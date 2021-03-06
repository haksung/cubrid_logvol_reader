[로그 헤더에 대한 구조체]
=> 로그 헤더 구조체를 통해서, 역할을 유추 해볼 수 있다.

/*
 * LOG HEADER INFORMATION
 */
typedef struct log_header LOG_HEADER;
struct log_header
{				/* Log header information */
  char magic[CUBRID_MAGIC_MAX_LENGTH];	/* Magic value for file/magic Unix
					 * utility
					 */
  INT32 dummy;			/* for 8byte align */
  INT64 db_creation;		/* Database creation time. For safety reasons,
				 * this value is set on all volumes and the
				 * log. The value is generated by the log
				 * manager
				 */
  char db_release[REL_MAX_RELEASE_LENGTH];	/* CUBRID Release */
  float db_compatibility;	/* Compatibility of the database against the
				 * current release of CUBRID
				 */
  PGLENGTH db_iopagesize;	/* Size of pages in the database. For safety
				 * reasons this value is recorded in the log
				 * to make sure that the database is always
				 * run with the same page size
				 */
  PGLENGTH db_logpagesize;	/* Size of log pages in the database. */
  int is_shutdown;		/* Was the log shutdown ?                   */
  TRANID next_trid;		/* Next Transaction identifier              */
  int avg_ntrans;		/* Number of average transactions           */
  int avg_nlocks;		/* Average number of object locks           */
  DKNPAGES npages;		/* Number of pages in the active log portion.
				 * Does not include the log header page.
				 */
  INT8 db_charset;
  INT8 dummy2;			/* Dummy fields for 8byte align */
  INT8 dummy3;
  INT8 dummy4;
  LOG_PAGEID fpageid;		/* Logical pageid at physical location 1 in
				 * active log
				 */
  LOG_LSA append_lsa;		/* Current append location                  */
  LOG_LSA chkpt_lsa;		/* Lowest log sequence address to start the
				 * recovery process
				 */
  LOG_PAGEID nxarv_pageid;	/* Next logical page to archive             */
  LOG_PHY_PAGEID nxarv_phy_pageid;	/* Physical location of logical page to
					 * archive
					 */
  int nxarv_num;		/* Next log archive number                  */
  int last_arv_num_for_syscrashes;	/* Last log archive needed for system
					 * crashes
					 */
  int last_deleted_arv_num;	/* Last deleted archive number              */
  LOG_LSA bkup_level0_lsa;	/* Lsa of backup level 0                    */
  LOG_LSA bkup_level1_lsa;	/* Lsa of backup level 1                    */
  LOG_LSA bkup_level2_lsa;	/* Lsa of backup level 2                    */
  char prefix_name[MAXLOGNAME];	/* Log prefix name                          */
  bool has_logging_been_skipped;	/* Has logging been skipped ?       */
  int reserved_int_1;		/* for backward compitablity
				 * - previously used for lowest_arv_num_for_backup */
  int reserved_int_2;		/* for backward compitablity
				 * - previously used for highest_arv_num_for_backup */
  int perm_status;		/* Reserved for future expansion and
				 * permanent status indicators,
				 * e.g. to mark RESTORE_IN_PROGRESS
				 */
  LOG_HDR_BKUP_LEVEL_INFO bkinfo[FILEIO_BACKUP_UNDEFINED_LEVEL];
  /* backup specific info
   * for future growth
   */

  int ha_server_state;
  int ha_file_status;
  LOG_LSA eof_lsa;

  LOG_LSA smallest_lsa_at_last_chkpt;
};
