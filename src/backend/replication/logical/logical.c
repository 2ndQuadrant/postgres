/*-------------------------------------------------------------------------
 * logical.c
 *	   PostgreSQL logical decoding coordination
 *
 * Copyright (c) 2012-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logical.c
 *
 * NOTES
 *	  This file coordinates interaction between the various modules that
 *	  together provide logical decoding, primarily by providing so
 *	  called LogicalDecodingContexts. The goal is to encapsulate most of the
 *	  internal complexity for consumers of logical decoding, so they can
 *	  create and consume a changestream with a low amount of code. Builtin
 *	  consumers are the walsender and SQL SRF interface, but it's possible to
 *	  add further ones without changing core code, e.g. to consume changes in
 *	  a bgworker.
 *
 *	  The idea is that a consumer provides three callbacks, one to read WAL,
 *	  one to prepare a data write, and a final one for actually writing since
 *	  their implementation depends on the type of consumer.  Check
 *	  logicalfuncs.c for an example implementation of a fairly simple consumer
 *	  and an implementation of a WAL reading callback that's suitable for
 *	  simple consumers.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"

#include "access/xact.h"
#include "access/xlog_internal.h"

#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/origin.h"
#include "replication/snapbuild.h"
#include "replication/walreceiver.h"

#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"

#include "utils/memutils.h"
#include "utils/ps_status.h"

/* data for errcontext callback */
typedef struct LogicalErrorCallbackState
{
	LogicalDecodingContext *ctx;
	const char *callback_name;
	XLogRecPtr	report_location;
} LogicalErrorCallbackState;

/* wrappers around output plugin callbacks */
static void output_plugin_error_callback(void *arg);
static void startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				   bool is_init);
static void shutdown_cb_wrapper(LogicalDecodingContext *ctx);
static void begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn);
static void change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  Relation relation, ReorderBufferChange *change);
static void message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr message_lsn, bool transactional,
				 const char *prefix, Size message_size, const char *message);

static void LoadOutputPlugin(OutputPluginCallbacks *callbacks, char *plugin);

static void EnsureActiveLogicalSlotValid(void);

/*
 * Make sure the current settings & environment are capable of doing logical
 * decoding.
 */
void
CheckLogicalDecodingRequirements(void)
{
	CheckSlotRequirements();

	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires wal_level >= logical")));

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires a database connection")));

	/* ----
	 * TODO: We got to change that someday soon...
	 *
	 * There's basically three things missing to allow this:
	 * 1) We need to be able to correctly and quickly identify the timeline a
	 *	  LSN belongs to
	 * 2) We need to force hot_standby_feedback to be enabled at all times so
	 *	  the primary cannot remove rows we need.
	 * 3) support dropping replication slots referring to a database, in
	 *	  dbase_redo. There can't be any active ones due to HS recovery
	 *	  conflicts, so that should be relatively easy.
	 * ----
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("logical decoding cannot be used while in recovery")));
}

/*
 * Helper function for CreateInitialDecodingContext() and
 * CreateDecodingContext() performing common tasks.
 */
static LogicalDecodingContext *
StartupDecodingContext(List *output_plugin_options,
					   XLogRecPtr start_lsn,
					   TransactionId xmin_horizon,
					   XLogPageReadCB read_page,
					   LogicalOutputPluginWriterPrepareWrite prepare_write,
					   LogicalOutputPluginWriterWrite do_write)
{
	ReplicationSlot *slot;
	MemoryContext context,
				old_context;
	LogicalDecodingContext *ctx;

	/* shorter lines... */
	slot = MyReplicationSlot;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"Logical decoding context",
									ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	ctx = palloc0(sizeof(LogicalDecodingContext));

	ctx->context = context;

	/*
	 * (re-)load output plugins, so we detect a bad (removed) output plugin
	 * now.
	 */
	LoadOutputPlugin(&ctx->callbacks, NameStr(slot->data.plugin));

	/*
	 * Now that the slot's xmin has been set, we can announce ourselves as a
	 * logical decoding backend which doesn't need to be checked individually
	 * when computing the xmin horizon because the xmin is enforced via
	 * replication slots.
	 *
	 * We can only do so if we're outside of a transaction (i.e. the case when
	 * streaming changes via walsender), otherwise an already setup
	 * snapshot/xid would end up being ignored. That's not a particularly
	 * bothersome restriction since the SQL interface can't be used for
	 * streaming anyway.
	 */
	if (!IsTransactionOrTransactionBlock())
	{
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyPgXact->vacuumFlags |= PROC_IN_LOGICAL_DECODING;
		LWLockRelease(ProcArrayLock);
	}

	ctx->slot = slot;

	ctx->reader = XLogReaderAllocate(read_page, ctx);
	if (!ctx->reader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	ctx->reader->private_data = ctx;

	ctx->reorder = ReorderBufferAllocate();
	ctx->snapshot_builder =
		AllocateSnapshotBuilder(ctx->reorder, xmin_horizon, start_lsn);

	ctx->reorder->private_data = ctx;

	/* wrap output plugin callbacks, so we can add error context information */
	ctx->reorder->begin = begin_cb_wrapper;
	ctx->reorder->apply_change = change_cb_wrapper;
	ctx->reorder->commit = commit_cb_wrapper;
	ctx->reorder->message = message_cb_wrapper;

	ctx->out = makeStringInfo();
	ctx->prepare_write = prepare_write;
	ctx->write = do_write;

	ctx->output_plugin_options = output_plugin_options;

	MemoryContextSwitchTo(old_context);

	return ctx;
}

/*
 * Create a new decoding context, for a new logical slot.
 *
 * plugin contains the name of the output plugin
 * output_plugin_options contains options passed to the output plugin
 * read_page, prepare_write, do_write are callbacks that have to be filled to
 *		perform the use-case dependent, actual, work.
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 */
LogicalDecodingContext *
CreateInitDecodingContext(char *plugin,
						  List *output_plugin_options,
						  XLogPageReadCB read_page,
						  LogicalOutputPluginWriterPrepareWrite prepare_write,
						  LogicalOutputPluginWriterWrite do_write)
{
	TransactionId xmin_horizon = InvalidTransactionId;
	ReplicationSlot *slot;
	LogicalDecodingContext *ctx;
	MemoryContext old_context;

	/* shorter lines... */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	if (plugin == NULL)
		elog(ERROR, "cannot initialize logical decoding without a specified plugin");

	/* Make sure the passed slot is suitable. These are user facing errors. */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		errmsg("cannot use physical replication slot for logical decoding")));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		   errmsg("replication slot \"%s\" was not created in this database",
				  NameStr(slot->data.name))));

	if (IsTransactionState() &&
		GetTopTransactionIdIfAny() != InvalidTransactionId)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot create logical replication slot in transaction that has performed writes")));

	/* register output plugin name with slot */
	SpinLockAcquire(&slot->mutex);
	StrNCpy(NameStr(slot->data.plugin), plugin, NAMEDATALEN);
	SpinLockRelease(&slot->mutex);

	ReplicationSlotReserveWal();

	/* ----
	 * This is a bit tricky: We need to determine a safe xmin horizon to start
	 * decoding from, to avoid starting from a running xacts record referring
	 * to xids whose rows have been vacuumed or pruned
	 * already. GetOldestSafeDecodingTransactionId() returns such a value, but
	 * without further interlock its return value might immediately be out of
	 * date.
	 *
	 * So we have to acquire the ProcArrayLock to prevent computation of new
	 * xmin horizons by other backends, get the safe decoding xid, and inform
	 * the slot machinery about the new limit. Once that's done the
	 * ProcArrayLock can be released as the slot machinery now is
	 * protecting against vacuum.
	 * ----
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	slot->effective_catalog_xmin = GetOldestSafeDecodingTransactionId();
	slot->data.catalog_xmin = slot->effective_catalog_xmin;

	ReplicationSlotsComputeRequiredXmin(true);

	LWLockRelease(ProcArrayLock);

	/*
	 * If this is the first slot created on the master we won't have a
	 * persistent record of the oldest safe xid for historic snapshots yet.
	 * Force one to be recorded so that when we go to replay from this slot we
	 * know it's safe.
	 */
	if (!RecoveryInProgress() &&
		!TransactionIdIsValid(ShmemVariableCache->oldestCatalogXmin))
		UpdateOldestCatalogXmin();

	/*
	 * tell the snapshot builder to only assemble snapshot once reaching the
	 * running_xact's record with the respective xmin.
	 */
	xmin_horizon = slot->data.catalog_xmin;

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();

	ctx = StartupDecodingContext(NIL, InvalidXLogRecPtr, xmin_horizon,
								 read_page, prepare_write, do_write);

	/* call output plugin initialization callback */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, true);
	MemoryContextSwitchTo(old_context);

	return ctx;
}

/*
 * Create a new decoding context, for a logical slot that has previously been
 * used already.
 *
 * start_lsn
 *		The LSN at which to start decoding.  If InvalidXLogRecPtr, restart
 *		from the slot's confirmed_flush; otherwise, start from the specified
 *		location (but move it forwards to confirmed_flush if it's older than
 *		that, see below).
 *
 * output_plugin_options
 *		contains options passed to the output plugin.
 *
 * read_page, prepare_write, do_write
 *		callbacks that have to be filled to perform the use-case dependent,
 *		actual work.
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 */
LogicalDecodingContext *
CreateDecodingContext(XLogRecPtr start_lsn,
					  List *output_plugin_options,
					  XLogPageReadCB read_page,
					  LogicalOutputPluginWriterPrepareWrite prepare_write,
					  LogicalOutputPluginWriterWrite do_write)
{
	LogicalDecodingContext *ctx;
	ReplicationSlot *slot;
	MemoryContext old_context;

	/* shorter lines... */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	/* make sure the passed slot is suitable, these are user facing errors */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 (errmsg("cannot use physical replication slot for logical decoding"))));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		  (errmsg("replication slot \"%s\" was not created in this database",
				  NameStr(slot->data.name)))));

	if (start_lsn == InvalidXLogRecPtr)
	{
		/* continue from last position */
		start_lsn = slot->data.confirmed_flush;
	}
	else if (start_lsn < slot->data.confirmed_flush)
	{
		/*
		 * It might seem like we should error out in this case, but it's
		 * pretty common for a client to acknowledge a LSN it doesn't have to
		 * do anything for, and thus didn't store persistently, because the
		 * xlog records didn't result in anything relevant for logical
		 * decoding. Clients have to be able to do that to support synchronous
		 * replication.
		 */
		elog(DEBUG1, "cannot stream from %X/%X, minimum is %X/%X, forwarding",
			 (uint32) (start_lsn >> 32), (uint32) start_lsn,
			 (uint32) (slot->data.confirmed_flush >> 32),
			 (uint32) slot->data.confirmed_flush);

		start_lsn = slot->data.confirmed_flush;
	}

	EnsureActiveLogicalSlotValid();

	ctx = StartupDecodingContext(output_plugin_options,
								 start_lsn, InvalidTransactionId,
								 read_page, prepare_write, do_write);

	/* call output plugin initialization callback */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, false);
	MemoryContextSwitchTo(old_context);

	ereport(LOG,
			(errmsg("starting logical decoding for slot \"%s\"",
					NameStr(slot->data.name)),
			 errdetail("streaming transactions committing after %X/%X, reading WAL from %X/%X",
					   (uint32) (slot->data.confirmed_flush >> 32),
					   (uint32) slot->data.confirmed_flush,
					   (uint32) (slot->data.restart_lsn >> 32),
					   (uint32) slot->data.restart_lsn)));

	return ctx;
}

/*
 * Returns true if a consistent initial decoding snapshot has been built.
 */
bool
DecodingContextReady(LogicalDecodingContext *ctx)
{
	return SnapBuildCurrentState(ctx->snapshot_builder) == SNAPBUILD_CONSISTENT;
}

/*
 * Read from the decoding slot, until it is ready to start extracting changes.
 */
void
DecodingContextFindStartpoint(LogicalDecodingContext *ctx)
{
	XLogRecPtr	startptr;

	/* Initialize from where to start reading WAL. */
	startptr = ctx->slot->data.restart_lsn;

	elog(DEBUG1, "searching for logical decoding starting point, starting at %X/%X",
		 (uint32) (ctx->slot->data.restart_lsn >> 32),
		 (uint32) ctx->slot->data.restart_lsn);

	/* Wait for a consistent starting point */
	for (;;)
	{
		XLogRecord *record;
		char	   *err = NULL;

		/* the read_page callback waits for new WAL */
		record = XLogReadRecord(ctx->reader, startptr, &err);
		if (err)
			elog(ERROR, "%s", err);
		if (!record)
			elog(ERROR, "no record found");		/* shouldn't happen */

		startptr = InvalidXLogRecPtr;

		LogicalDecodingProcessRecord(ctx, ctx->reader);

		/* only continue till we found a consistent spot */
		if (DecodingContextReady(ctx))
			break;

		CHECK_FOR_INTERRUPTS();
	}

	ctx->slot->data.confirmed_flush = ctx->reader->EndRecPtr;
}

/*
 * Free a previously allocated decoding context, invoking the shutdown
 * callback if necessary.
 */
void
FreeDecodingContext(LogicalDecodingContext *ctx)
{
	if (ctx->callbacks.shutdown_cb != NULL)
		shutdown_cb_wrapper(ctx);

	ReorderBufferFree(ctx->reorder);
	FreeSnapshotBuilder(ctx->snapshot_builder);
	XLogReaderFree(ctx->reader);
	MemoryContextDelete(ctx->context);
}

/*
 * Prepare a write using the context's output routine.
 */
void
OutputPluginPrepareWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->accept_writes)
		elog(ERROR, "writes are only accepted in commit, begin and change callbacks");

	ctx->prepare_write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = true;
}

/*
 * Perform a write using the context's output routine.
 */
void
OutputPluginWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->prepared_write)
		elog(ERROR, "OutputPluginPrepareWrite needs to be called before OutputPluginWrite");

	ctx->write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = false;
}

/*
 * Load the output plugin, lookup its output plugin init function, and check
 * that it provides the required callbacks.
 */
static void
LoadOutputPlugin(OutputPluginCallbacks *callbacks, char *plugin)
{
	LogicalOutputPluginInit plugin_init;

	plugin_init = (LogicalOutputPluginInit)
		load_external_function(plugin, "_PG_output_plugin_init", false, NULL);

	if (plugin_init == NULL)
		elog(ERROR, "output plugins have to declare the _PG_output_plugin_init symbol");

	/* ask the output plugin to fill the callback struct */
	plugin_init(callbacks);

	if (callbacks->begin_cb == NULL)
		elog(ERROR, "output plugins have to register a begin callback");
	if (callbacks->change_cb == NULL)
		elog(ERROR, "output plugins have to register a change callback");
	if (callbacks->commit_cb == NULL)
		elog(ERROR, "output plugins have to register a commit callback");
}

static void
output_plugin_error_callback(void *arg)
{
	LogicalErrorCallbackState *state = (LogicalErrorCallbackState *) arg;

	/* not all callbacks have an associated LSN  */
	if (state->report_location != InvalidXLogRecPtr)
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback, associated LSN %X/%X",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name,
				   (uint32) (state->report_location >> 32),
				   (uint32) state->report_location);
	else
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name);
}

static void
startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "startup";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;

	/* do the actual work: call callback */
	ctx->callbacks.startup_cb(ctx, opt, is_init);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
shutdown_cb_wrapper(LogicalDecodingContext *ctx)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "shutdown";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;

	/* do the actual work: call callback */
	ctx->callbacks.shutdown_cb(ctx);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}


/*
 * Callbacks for ReorderBuffer which add in some more information and then call
 * output_plugin.h plugins.
 */
static void
begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "begin";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;

	/* do the actual work: call callback */
	ctx->callbacks.begin_cb(ctx, txn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "commit";
	state.report_location = txn->final_lsn;		/* beginning of commit record */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record */

	/* do the actual work: call callback */
	ctx->callbacks.commit_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

static void
change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * report this change's lsn so replies from clients can give an up2date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 */
	ctx->write_location = change->lsn;

	ctx->callbacks.change_cb(ctx, txn, relation, change);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

bool
filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "filter_by_origin";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = false;

	/* do the actual work: call callback */
	ret = ctx->callbacks.filter_by_origin_cb(ctx, origin_id);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	return ret;
}

static void
message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr message_lsn, bool transactional,
				   const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	if (ctx->callbacks.message_cb == NULL)
		return;

	/* Push callback + info on the error context stack */
	state.ctx = ctx;
	state.callback_name = "message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = (void *) &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;

	/* do the actual work: call callback */
	ctx->callbacks.message_cb(ctx, txn, message_lsn, transactional, prefix,
							  message_size, message);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}

/*
 * Set the required catalog xmin horizon for historic snapshots in the current
 * replication slot.
 *
 * Note that in the most cases, we won't be able to immediately use the xmin
 * to increase the xmin horizon: we need to wait till the client has confirmed
 * receiving current_lsn with LogicalConfirmReceivedLocation().
 */
void
LogicalIncreaseXminForSlot(XLogRecPtr current_lsn, TransactionId xmin)
{
	bool		updated_xmin = false;
	ReplicationSlot *slot;

	slot = MyReplicationSlot;

	Assert(slot != NULL);

	SpinLockAcquire(&slot->mutex);

	/*
	 * don't overwrite if we already have a newer xmin. This can happen if we
	 * restart decoding in a slot.
	 */
	if (TransactionIdPrecedesOrEquals(xmin, slot->data.catalog_xmin))
	{
	}

	/*
	 * If the client has already confirmed up to this lsn, we directly can
	 * mark this as accepted. This can happen if we restart decoding in a
	 * slot.
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/* our candidate can directly be used */
		updated_xmin = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly.
	 */
	else if (slot->candidate_xmin_lsn == InvalidXLogRecPtr)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;
	}
	SpinLockRelease(&slot->mutex);

	/* candidate already valid with the current flush position, apply */
	if (updated_xmin)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Mark the minimal LSN (restart_lsn) we need to read to replay all
 * transactions that have not yet committed at current_lsn.
 *
 * Just like IncreaseRestartDecodingForSlot this only takes effect when the
 * client has confirmed to have received current_lsn.
 */
void
LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn, XLogRecPtr restart_lsn)
{
	bool		updated_lsn = false;
	ReplicationSlot *slot;

	slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(restart_lsn != InvalidXLogRecPtr);
	Assert(current_lsn != InvalidXLogRecPtr);

	SpinLockAcquire(&slot->mutex);

	/* don't overwrite if have a newer restart lsn */
	if (restart_lsn <= slot->data.restart_lsn)
	{
	}

	/*
	 * We might have already flushed far enough to directly accept this lsn,
	 * in this case there is no need to check for existing candidate LSNs
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;

		/* our candidate can directly be used */
		updated_lsn = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly. A missed
	 * value here will just cause some extra effort after reconnecting.
	 */
	if (slot->candidate_restart_valid == InvalidXLogRecPtr)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;

		elog(DEBUG1, "got new restart lsn %X/%X at %X/%X",
			 (uint32) (restart_lsn >> 32), (uint32) restart_lsn,
			 (uint32) (current_lsn >> 32), (uint32) current_lsn);
	}
	else
	{
		elog(DEBUG1, "failed to increase restart lsn: proposed %X/%X, after %X/%X, current candidate %X/%X, current after %X/%X, flushed up to %X/%X",
			 (uint32) (restart_lsn >> 32), (uint32) restart_lsn,
			 (uint32) (current_lsn >> 32), (uint32) current_lsn,
			 (uint32) (slot->candidate_restart_lsn >> 32),
			 (uint32) slot->candidate_restart_lsn,
			 (uint32) (slot->candidate_restart_valid >> 32),
			 (uint32) slot->candidate_restart_valid,
			 (uint32) (slot->data.confirmed_flush >> 32),
			 (uint32) slot->data.confirmed_flush
			);
	}
	SpinLockRelease(&slot->mutex);

	/* candidates are already valid with the current flush position, apply */
	if (updated_lsn)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Handle a consumer's confirmation having received all changes up to lsn.
 */
void
LogicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	Assert(lsn != InvalidXLogRecPtr);

	/* Do an unlocked check for candidate_lsn first. */
	if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr ||
		MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr)
	{
		bool		updated_xmin = false;
		bool		updated_restart = false;

		SpinLockAcquire(&MyReplicationSlot->mutex);

		MyReplicationSlot->data.confirmed_flush = lsn;

		/* if we're past the location required for bumping xmin, do so */
		if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_xmin_lsn <= lsn)
		{
			/*
			 * We have to write the changed xmin to disk *before* we change
			 * the in-memory value, otherwise after a crash we wouldn't know
			 * that some catalog tuples might have been removed already.
			 *
			 * Ensure that by first writing to ->xmin and only update
			 * ->effective_xmin once the new state is synced to disk. After a
			 * crash ->effective_xmin is set to ->xmin.
			 */
			if (TransactionIdIsValid(MyReplicationSlot->candidate_catalog_xmin) &&
				MyReplicationSlot->data.catalog_xmin != MyReplicationSlot->candidate_catalog_xmin)
			{
				MyReplicationSlot->data.catalog_xmin = MyReplicationSlot->candidate_catalog_xmin;
				MyReplicationSlot->candidate_catalog_xmin = InvalidTransactionId;
				MyReplicationSlot->candidate_xmin_lsn = InvalidXLogRecPtr;
				updated_xmin = true;
			}
		}

		if (MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_restart_valid <= lsn)
		{
			Assert(MyReplicationSlot->candidate_restart_lsn != InvalidXLogRecPtr);

			MyReplicationSlot->data.restart_lsn = MyReplicationSlot->candidate_restart_lsn;
			MyReplicationSlot->candidate_restart_lsn = InvalidXLogRecPtr;
			MyReplicationSlot->candidate_restart_valid = InvalidXLogRecPtr;
			updated_restart = true;
		}

		SpinLockRelease(&MyReplicationSlot->mutex);

		/* first write new xmin to disk, so we know what's up after a crash */
		if (updated_xmin || updated_restart)
		{
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			elog(DEBUG1, "updated xmin: %u restart: %u", updated_xmin, updated_restart);
		}

		/*
		 * Now the new xmin is safely on disk, we can let the global value
		 * advance. We do not take ProcArrayLock or similar since we only
		 * advance xmin here and there's not much harm done by a concurrent
		 * computation missing that.
		 */
		if (updated_xmin)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->effective_catalog_xmin = MyReplicationSlot->data.catalog_xmin;
			SpinLockRelease(&MyReplicationSlot->mutex);

			ReplicationSlotsComputeRequiredXmin(false);
			ReplicationSlotsComputeRequiredLSN();
		}
	}
	else
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.confirmed_flush = lsn;
		SpinLockRelease(&MyReplicationSlot->mutex);
	}
}

/*
 * Test to see if the active logical slot is usable.
 */
static void
EnsureActiveLogicalSlotValid(void)
{
	TransactionId shmem_catalog_xmin;

	Assert(MyReplicationSlot != NULL);

	/*
	 * A logical slot can become unusable if we're doing logical decoding on a
	 * standby or using a slot created before we were promoted from standby
	 * to master. If the master advanced its global catalog_xmin past the
	 * threshold we need it could've removed catalog tuple versions that
	 * we'll require to start decoding at our restart_lsn.
	 */

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	shmem_catalog_xmin = ShmemVariableCache->oldestCatalogXmin;
	LWLockRelease(ProcArrayLock);

	if (!TransactionIdIsValid(shmem_catalog_xmin) ||
		TransactionIdFollows(shmem_catalog_xmin, MyReplicationSlot->data.catalog_xmin))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot '%s' requires catalogs removed by master",
						NameStr(MyReplicationSlot->data.name)),
				 errdetail("need catalog_xmin %u, have oldestCatalogXmin %u",
						   MyReplicationSlot->data.catalog_xmin, shmem_catalog_xmin)));
}

/*
 * Scan to see if any clients are using replication slots that are below a
 * newly-applied new catalog_xmin theshold and signal them to terminate with a
 * recovery conflict.
 */
void
ResolveRecoveryConflictWithLogicalDecoding(TransactionId new_catalog_xmin)
{
	int i;

	if (!InHotStandby)
		/* nobody can be actively using logical slots */
		return;

	/* Already applied new limit, can't have replayed later one yet */
	Assert(ShmemVariableCache->oldestCatalogXmin == new_catalog_xmin);

	/*
	 * Find the first conflicting active slot and signal its owning backend
	 * to exit. We'll be called repeatedly by the recovery code until there
	 * are no more conflicts.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot;
		pid_t active_pid;

		slot = &ReplicationSlotCtl->replication_slots[i];

		/*
		 * Physical slots can have a catalog_xmin, but conflicts are the
		 * problem of the leaf replica with the logical slot.
		 */
		if (!(slot->in_use && SlotIsLogical(slot)))
			continue;

		/*
		 * We only care about the effective_catalog_xmin of active logical
		 * slots. Anything else gets checked when a new decoding session tries
		 * to start.
		 */
		 while (slot->in_use && slot->active_pid != 0 &&
				TransactionIdIsValid(slot->effective_catalog_xmin) &&
				(!TransactionIdIsValid(new_catalog_xmin) ||
				 TransactionIdPrecedes(slot->effective_catalog_xmin, new_catalog_xmin)))
		{
			/*
			 * We'll be sleeping, so release the control lock. New conflicting
			 * backends cannot appear and if old ones go away that's what we
			 * want, so release and re-acquire is OK here.
			 */
			active_pid = slot->active_pid;
			LWLockRelease(ReplicationSlotControlLock);

			if (WaitExceedsMaxStandbyDelay())
			{
				ereport(INFO,
						(errmsg("terminating logical decoding session due to recovery conflict"),
						 errdetail("Pid %u requires catalog_xmin %u for replication slot '%s' but the master has removed catalogs up to xid %u.",
								   active_pid, slot->effective_catalog_xmin,
								   NameStr(slot->data.name), new_catalog_xmin)));

				/*
				 * Signal the proc. If the slot is already released or even if
				 * pid is re-used we don't care, backends are required to
				 * tolerate spurious recovery signals.
				 */
				CancelLogicalDecodingSessionWithRecoveryConflict(active_pid);

				/* Don't flood the system with signals */
				pg_usleep(10000);
			}

			/*
			 * We need to re-acquire the lock before re-checking the slot or
			 * continuing the scan.
			 */
			LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
		}

	}
	LWLockRelease(ReplicationSlotControlLock);
}
