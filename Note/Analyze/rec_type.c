[Record type에 대한 선언]
=> 로그의 역할에 대해 enum으로 선언해 놓음.
=> 이를 확인해서, 해당 로그의 역할을 파악할 수 있다.
=> 보기 편하게 하려고, c 파일로 저장.

typedef enum log_rectype LOG_RECTYPE;
enum log_rectype
{
  /* In order of likely of appearance in the log */
  LOG_SMALLER_LOGREC_TYPE = 0,	/* A lower bound check             */

  LOG_CLIENT_NAME = 1,		/* Name of the client associated
				   with transaction
				 */
  LOG_UNDOREDO_DATA = 2,	/* An undo and redo data record    */
  LOG_UNDO_DATA = 3,		/* Only undo data                  */
  LOG_REDO_DATA = 4,		/* Only redo data                  */
  LOG_DBEXTERN_REDO_DATA = 5,	/* Only database external redo data */
  LOG_POSTPONE = 6,		/* Postpone redo data              */
  LOG_RUN_POSTPONE = 7,		/* Run/redo a postpone data. Only
				   for transactions committed with
				   postpone operations
				 */
  LOG_COMPENSATE = 8,		/* Compensation record (compensate a
				   undo record of an aborted tran)
				 */
  LOG_LCOMPENSATE = 9,		/* Compensation record (compensate a
				   logical undo of an aborted tran)
				 */
  LOG_CLIENT_USER_UNDO_DATA = 10,	/* User client undo data           */
  LOG_CLIENT_USER_POSTPONE_DATA = 11,	/* User client postpone            */
  LOG_RUN_NEXT_CLIENT_UNDO = 12,	/* Used to indicate that a set of
					   client undo operations has
					   been executed and the address of
					   the next client undo to execute
					 */
  LOG_RUN_NEXT_CLIENT_POSTPONE = 13,	/* Used to indicate that a set of
					   client postpone operations has
					   been executed and the address of
					   the next client postpone to
					   execute
					 */
  LOG_WILL_COMMIT = 14,		/* Transaction will be committed */
  LOG_COMMIT_WITH_POSTPONE = 15,	/* Committing server postpone
					   operations
					 */
  LOG_COMMIT_WITH_CLIENT_USER_LOOSE_ENDS = 16,	/* Committing client postpone
						   operations
						 */
  LOG_COMMIT = 17,		/* A commit record                 */
  LOG_COMMIT_TOPOPE_WITH_POSTPONE = 18,	/* Committing server top system
					   postpone operations
					 */
  LOG_COMMIT_TOPOPE_WITH_CLIENT_USER_LOOSE_ENDS = 19,
  /* Committing client postpone
     top system operations
   */
  LOG_COMMIT_TOPOPE = 20,	/* A partial commit record, usually
				   from a top system operation
				 */
  LOG_ABORT_WITH_CLIENT_USER_LOOSE_ENDS = 21,	/* Aborting client loose ends      */
  LOG_ABORT = 22,		/* An abort record                 */
  LOG_ABORT_TOPOPE_WITH_CLIENT_USER_LOOSE_ENDS = 23,
  /* Committing client postpone
     top system operations
   */
  LOG_ABORT_TOPOPE = 24,	/* A partial abort record, usually
				   from a top system operation or
				   partial rollback to a save point
				 */
  LOG_START_CHKPT = 25,		/* Start a checkpoint              */
  LOG_END_CHKPT = 26,		/* Checkpoint information          */
  LOG_SAVEPOINT = 27,		/* A user savepoint record         */
  LOG_2PC_PREPARE = 28,		/* A prepare to commit record      */
  LOG_2PC_START = 29,		/* Start the 2PC protocol by sending
				   vote request messages to
				   participants of distributed tran.
				 */
  LOG_2PC_COMMIT_DECISION = 30,	/* Beginning of the second phase of
				   2PC, need to perform local &
				   global commits.
				 */
  LOG_2PC_ABORT_DECISION = 31,	/* Beginning of the second phase of
				   2PC, need to perform local &
				   global aborts.
				 */
  LOG_2PC_COMMIT_INFORM_PARTICPS = 32,	/* Committing, need to inform the
					   participants
					 */
  LOG_2PC_ABORT_INFORM_PARTICPS = 33,	/* Aborting, need to inform the
					   participants
					 */
  LOG_2PC_RECV_ACK = 34,	/* Received ack. from the
				   participant that it received the
				   decision on the fate of dist.
				   trans.
				 */
  LOG_END_OF_LOG = 35,		/* End of log                      */
  LOG_DUMMY_HEAD_POSTPONE = 36,	/* A dummy log record. No-op       */
  LOG_DUMMY_CRASH_RECOVERY = 37,	/* A dummy log record which indicate
					   the start of crash recovery.
					   No-op
					 */

  LOG_DUMMY_FILLPAGE_FORARCHIVE = 38,	/* Indicates logical end of current
					   page so it could be archived
					   safely. No-op
					   This record is not generated no more.
					   It's kept for backward compatibility.
					 */
  LOG_REPLICATION_DATA = 39,	/* Replicaion log for insert, delete or update */
  LOG_REPLICATION_SCHEMA = 40,	/* Replicaion log for schema, index, trigger or system catalog updates */
  LOG_UNLOCK_COMMIT = 41,	/* for repl_agent to guarantee the order of */
  LOG_UNLOCK_ABORT = 42,	/* transaction commit, we append the unlock info.
				   before calling lock_unlock_all()
				 */
  LOG_DIFF_UNDOREDO_DATA = 43,	/* diff undo redo data             */
  LOG_DUMMY_HA_SERVER_STATE = 44,	/* HA server state */
  LOG_DUMMY_OVF_RECORD = 45,	/* indicator of the first part of an overflow record */
  LOG_LARGER_LOGREC_TYPE	/* A higher bound for checks       */
};
