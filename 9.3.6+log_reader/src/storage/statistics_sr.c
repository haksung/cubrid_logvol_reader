/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * statistics_sr.c - statistics manager (server)
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "xserver_interface.h"
#include "memory_alloc.h"
#include "statistics_sr.h"
#include "object_representation.h"
#include "error_manager.h"
#include "storage_common.h"
#include "system_catalog.h"
#include "btree.h"
#include "extendible_hash.h"
#include "heap_file.h"
#include "partition.h"
#include "db.h"

#define SQUARE(n) ((n)*(n))

/* Used by the "stats_update_all_statistics" routine to create the list of all
   classes from the extendible hashing directory used by the catalog manager. */
typedef struct class_id_list CLASS_ID_LIST;
struct class_id_list
{
  OID class_id;
  CLASS_ID_LIST *next;
};

typedef struct partition_stats_acumulator PARTITION_STATS_ACUMULATOR;
struct partition_stats_acumulator
{
  double leafs;			/* number of leaf pages including overflow pages */
  double pages;			/* number of total pages */
  double height;		/* the height of the B+tree */
  double keys;			/* number of keys */
  int pkeys_size;		/* pkeys array size */
  double *pkeys;		/* partial keys info
				   for example: index (a, b, ..., x)
				   pkeys[0]          -> # of {a}
				   pkeys[1]          -> # of {a, b}
				   ...
				   pkeys[pkeys_size-1] -> # of {a, b, ..., x}
				 */
};

static void stats_free_class_list (CLASS_ID_LIST * clsid_list);
static int stats_get_class_list (THREAD_ENTRY * thread_p, void *key,
				 void *val, void *args);
static int stats_compare_data (DB_DATA * data1, DB_DATA * data2,
			       DB_TYPE type);
static int stats_compare_date (DB_DATE * date1, DB_DATE * date2);
static int stats_compare_time (DB_TIME * time1, DB_TIME * time2);
static int stats_compare_utime (DB_UTIME * utime1, DB_UTIME * utime2);
static int stats_compare_datetime (DB_DATETIME * datetime1_p,
				   DB_DATETIME * datetime2_p);
static int stats_compare_money (DB_MONETARY * mn1, DB_MONETARY * mn2);
static int stats_update_partitioned_statistics (THREAD_ENTRY * thread_p,
						OID * class_oid,
						OID * partitions, int count,
						bool with_fullscan);

/*
 * xstats_update_statistics () -  Updates the statistics for the objects
 *                                of a given class
 *   return:
 *   class_id(in): Identifier of the class
 *   btids(in):
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * Note: It first retrieves the whole catalog information about this class,
 *       including all possible forms of disk representations for the instance
 *       objects. Then, it performs a complete pass on the heap file of the
 *       class, reading in all of the instance objects one by one and
 *       calculating the ranges of numeric attribute values (ie. min. & max.
 *       values for each numeric attribute).
 *
 *       During this pass on the heap file, these values are maintained
 *       separately for objects with the same representation. Each minimum and
 *       maximum value is initialized when the first instance of the class
 *       with the corresponding representation is encountered. These values are
 *       continually updated as attribute values exceeding the known range are
 *       encountered. At the end of this pass, these individual ranges for
 *       each representation are uniformed in the last (the current)
 *       representation, building the global range values for the attributes
 *       of the class. Then, the btree statistical information is obtained for
 *       each attribute that is indexed and stored in this final representation
 *       structure. Finally, a new timestamp is obtained for these class
 *       statistics and they are stored to disk within the catalog structure
 *       for the last class representation.
 */
int
xstats_update_statistics (THREAD_ENTRY * thread_p, OID * class_id_p,
			  BTID_LIST * btids, bool with_fullscan)
{
  CLS_INFO *cls_info_p = NULL;
  REPR_ID repr_id;
  DISK_REPR *disk_repr_p = NULL;
  DISK_ATTR *disk_attr_p = NULL;
  BTREE_STATS *btree_stats_p = NULL;
  int npages, estimated_nobjs;
  char *class_name = NULL;
  int i, j;
  OID *partitions = NULL;
  int count = 0, error_code = NO_ERROR;

  cls_info_p = catalog_get_class_info (thread_p, class_id_p);
  if (cls_info_p == NULL)
    {
      goto error;
    }

  class_name = heap_get_class_name (thread_p, class_id_p);

  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
	  ER_LOG_STARTED_TO_UPDATE_STATISTICS, 4,
	  class_name ? class_name : "*UNKNOWN-CLASS*",
	  class_id_p->volid, class_id_p->pageid, class_id_p->slotid);

  /* if class information was not obtained */
  if (cls_info_p->hfid.vfid.fileid < 0 || cls_info_p->hfid.vfid.volid < 0)
    {
      /* The class does not have a heap file (i.e. it has no instances);
         so no statistics can be obtained for this class;
         just set to 0 and return. */

      cls_info_p->tot_pages = 0;
      cls_info_p->tot_objects = 0;

      if (catalog_add_class_info (thread_p, class_id_p, cls_info_p) !=
	  NO_ERROR)
	{
	  goto error;
	}

      goto end;
    }

  error_code = partition_get_partition_oids (thread_p, class_id_p,
					     &partitions, &count);
  if (error_code != NO_ERROR)
    {
      goto error;
    }

  if (count != 0)
    {
      /* Update statistics for all partitions and the partitioned class */
      assert (partitions != NULL);
      catalog_free_class_info (cls_info_p);
      cls_info_p = NULL;
      error_code = stats_update_partitioned_statistics (thread_p,
							class_id_p,
							partitions, count,
							with_fullscan);
      db_private_free (thread_p, partitions);
      if (error_code != NO_ERROR)
	{
	  goto error;
	}

      goto end;
    }

  if (catalog_get_last_representation_id (thread_p, class_id_p, &repr_id) !=
      NO_ERROR)
    {
      goto error;
    }

  disk_repr_p = catalog_get_representation (thread_p, class_id_p, repr_id);
  if (disk_repr_p == NULL)
    {
      goto error;
    }

  npages = estimated_nobjs = 0;

  /* do not use estimated npages, get correct info */
  npages = file_get_numpages (thread_p, &(cls_info_p->hfid.vfid));
  cls_info_p->tot_pages = MAX (npages, 0);

  estimated_nobjs = heap_estimate_num_objects (thread_p, &(cls_info_p->hfid));
  if (estimated_nobjs == -1)
    {
      /* cannot get estimates from the heap, use old info */
      assert (cls_info_p->tot_objects >= 0);
    }
  else
    {
      cls_info_p->tot_objects = estimated_nobjs;
    }

  /* update the index statistics for each attribute */

  for (i = 0; i < disk_repr_p->n_fixed + disk_repr_p->n_variable; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  assert_release (!BTID_IS_NULL (&btree_stats_p->btid));
	  assert_release (btree_stats_p->pkeys_size > 0);
	  assert_release (btree_stats_p->pkeys_size <= BTREE_STATS_PKEYS_NUM);

	  if (btids)
	    {
	      BTID_LIST *b = btids;
	      while (b != NULL)
		{
		  if (!BTID_IS_EQUAL (&b->btid, &btree_stats_p->btid))
		    {
		      b = b->next;
		      continue;
		    }

		  if (btree_get_stats (thread_p, btree_stats_p,
				       with_fullscan) != NO_ERROR)
		    {
		      goto error;
		    }
		  break;
		}
	    }
	  else
	    {
	      if (btree_get_stats (thread_p, btree_stats_p,
				   with_fullscan) != NO_ERROR)
		{
		  goto error;
		}
	    }

	  assert_release (btree_stats_p->keys >= 0);
	}			/* for (j = 0; ...) */
    }				/* for (i = 0; ...) */

  /* replace the current disk representation structure/information in the
     catalog with the newly computed statistics */

  if (catalog_add_representation (thread_p, class_id_p, repr_id, disk_repr_p)
      != NO_ERROR)
    {
      goto error;
    }

  cls_info_p->time_stamp = stats_get_time_stamp ();

  if (catalog_add_class_info (thread_p, class_id_p, cls_info_p) != NO_ERROR)
    {
      goto error;
    }

end:
  if (disk_repr_p)
    {
      catalog_free_representation (disk_repr_p);
    }

  if (cls_info_p)
    {
      catalog_free_class_info (cls_info_p);
    }

  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
	  ER_LOG_FINISHED_TO_UPDATE_STATISTICS, 5,
	  class_name ? class_name : "*UNKNOWN-CLASS*",
	  class_id_p->volid, class_id_p->pageid, class_id_p->slotid,
	  error_code);

  if (class_name)
    {
      free_and_init (class_name);
    }

  return error_code;

error:

  if (error_code == NO_ERROR && (error_code = er_errid ()) == NO_ERROR)
    {
      error_code = ER_FAILED;
    }
  goto end;
}

/*
 * xstats_update_all_statistics () - Updates the statistics
 *                                   for all the classes of the database
 *   return:
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * Note: It performs this by getting the list of all classes existing in the
 *       database and their OID's from the catalog's class collection
 *       (maintained in an extendible hashing structure) and calling the
 *       "stats_update_statistics" function for each one of the elements
 *       of this list one by one.
 */
int
xstats_update_all_statistics (THREAD_ENTRY * thread_p, bool with_fullscan)
{
  int error;
  OID class_id;
  CLASS_ID_LIST *class_id_list_p = NULL, *class_id_item_p;

  ehash_map (thread_p, &catalog_Id.xhid, stats_get_class_list,
	     (void *) &class_id_list_p);

  for (class_id_item_p = class_id_list_p;
       class_id_item_p->next != NULL; class_id_item_p = class_id_item_p->next)
    {
      class_id.volid = class_id_item_p->class_id.volid;
      class_id.pageid = class_id_item_p->class_id.pageid;
      class_id.slotid = class_id_item_p->class_id.slotid;

      error =
	xstats_update_statistics (thread_p, &class_id, NULL, with_fullscan);
      if (error != NO_ERROR)
	{
	  stats_free_class_list (class_id_list_p);
	  return (error);
	}
    }

  stats_free_class_list (class_id_list_p);
  return (NO_ERROR);
}

/*
 * xstats_get_statistics_from_server () - Retrieves the class statistics
 *   return: buffer contaning class statistics, or NULL on error
 *   class_id(in): Identifier of the class
 *   timestamp(in):
 *   length(in): Length of the buffer
 *
 * Note: This function retrieves the statistics for the given class from the
 *       catalog manager and stores them into a buffer. Note that since the
 *       statistics are kept on the current (last) representation structure of
 *       the catalog, only this structure is retrieved. Note further that
 *       since the statistics are used only on the client side they are not
 *       put into a structure here on the server side (not even temporarily),
 *       but stored into a buffer area to be transmitted to the client side.
 */
char *
xstats_get_statistics_from_server (THREAD_ENTRY * thread_p, OID * class_id_p,
				   unsigned int time_stamp, int *length_p)
{
  CLS_INFO *cls_info_p;
  REPR_ID repr_id;
  DISK_REPR *disk_repr_p;
  DISK_ATTR *disk_attr_p;
  BTREE_STATS *btree_stats_p;
  int npages, estimated_nobjs, max_unique_keys;
  int i, j, k, size, n_attrs, tot_n_btstats, tot_key_info_size;
  char *buf_p, *start_p;
  int key_size;

  *length_p = -1;

  cls_info_p = catalog_get_class_info (thread_p, class_id_p);
  if (cls_info_p == NULL)
    {
      return NULL;
    }

  if (time_stamp > 0 && time_stamp >= cls_info_p->time_stamp)
    {
      catalog_free_class_info (cls_info_p);
      *length_p = 0;
      return NULL;
    }

  if (catalog_get_last_representation_id (thread_p, class_id_p, &repr_id) !=
      NO_ERROR)
    {
      catalog_free_class_info (cls_info_p);
      return NULL;
    }

  disk_repr_p = catalog_get_representation (thread_p, class_id_p, repr_id);
  if (disk_repr_p == NULL)
    {
      catalog_free_class_info (cls_info_p);
      return NULL;
    }

  n_attrs = disk_repr_p->n_fixed + disk_repr_p->n_variable;

  tot_n_btstats = tot_key_info_size = 0;
  for (i = 0; i < n_attrs; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      tot_n_btstats += disk_attr_p->n_btstats;
      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  tot_key_info_size +=
	    or_packed_domain_size (btree_stats_p->key_type, 0);
	  assert (btree_stats_p->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	  tot_key_info_size += (btree_stats_p->pkeys_size * OR_INT_SIZE);	/* pkeys[] */
	}
    }

  size = (OR_INT_SIZE		/* time_stamp of CLS_INFO */
	  + OR_INT_SIZE		/* tot_objects of CLS_INFO */
	  + OR_INT_SIZE		/* tot_pages of CLS_INFO */
	  + OR_INT_SIZE		/* n_attrs from DISK_REPR */
	  + (OR_INT_SIZE	/* id of DISK_ATTR */
	     + OR_INT_SIZE	/* type of DISK_ATTR */
	     + OR_INT_SIZE	/* n_btstats of DISK_ATTR */
	  ) * n_attrs);		/* number of attributes */

  size += ((OR_BTID_ALIGNED_SIZE	/* btid of BTREE_STATS */
	    + OR_INT_SIZE	/* leafs of BTREE_STATS */
	    + OR_INT_SIZE	/* pages of BTREE_STATS */
	    + OR_INT_SIZE	/* height of BTREE_STATS */
	    + OR_INT_SIZE	/* keys of BTREE_STATS */
	    + OR_INT_SIZE	/* does the BTREE_STATS correspond to a
				 * function index */
	   ) * tot_n_btstats);	/* total number of indexes */

  size += tot_key_info_size;	/* key_type, pkeys[] of BTREE_STATS */

  size += OR_INT_SIZE;		/* max_unique_keys */

  start_p = buf_p = (char *) malloc (size);
  if (buf_p == NULL)
    {
      catalog_free_representation (disk_repr_p);
      catalog_free_class_info (cls_info_p);
      return NULL;
    }
  memset (start_p, 0, size);

  OR_PUT_INT (buf_p, cls_info_p->time_stamp);
  buf_p += OR_INT_SIZE;

  npages = estimated_nobjs = max_unique_keys = 0;

  assert (cls_info_p->tot_objects >= 0);
  assert (cls_info_p->tot_pages >= 0);

  if (HFID_IS_NULL (&cls_info_p->hfid))
    {
      /* The class does not have a heap file (i.e. it has no instances);
       * so no statistics can be obtained for this class
       */
      OR_PUT_INT (buf_p, cls_info_p->tot_objects);	/* #objects */
      buf_p += OR_INT_SIZE;

      OR_PUT_INT (buf_p, cls_info_p->tot_pages);	/* #pages */
      buf_p += OR_INT_SIZE;
    }
  else
    {
      /* use estimates from the heap since it is likely that its estimates
       * are more accurate than the ones gathered at update statistics time
       */
      estimated_nobjs = heap_estimate_num_objects (thread_p,
						   &(cls_info_p->hfid));
      if (estimated_nobjs == -1)
	{
	  /* cannot get estimates from the heap, use ones from the catalog */
	  estimated_nobjs = cls_info_p->tot_objects;
	}
      else
	{
	  /* heuristic is that big nobjs is better than small */
	  estimated_nobjs = MAX (estimated_nobjs, cls_info_p->tot_objects);
	}

      OR_PUT_INT (buf_p, estimated_nobjs);	/* #objects */
      buf_p += OR_INT_SIZE;

      /* do not use estimated npages, get correct info */
      assert (!VFID_ISNULL (&cls_info_p->hfid.vfid));
      npages = file_get_numpages (thread_p, &cls_info_p->hfid.vfid);
      if (npages < 0)
	{
	  /* cannot get #pages from the heap, use ones from the catalog */
	  npages = cls_info_p->tot_pages;
	}

      OR_PUT_INT (buf_p, npages);	/* #pages */
      buf_p += OR_INT_SIZE;
    }

  OR_PUT_INT (buf_p, n_attrs);
  buf_p += OR_INT_SIZE;

  /* put the statistics information of each attribute to the buffer */
  for (i = 0; i < n_attrs; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      OR_PUT_INT (buf_p, disk_attr_p->id);
      buf_p += OR_INT_SIZE;

      OR_PUT_INT (buf_p, disk_attr_p->type);
      buf_p += OR_INT_SIZE;

      OR_PUT_INT (buf_p, disk_attr_p->n_btstats);
      buf_p += OR_INT_SIZE;

      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  /* collect maximum unique keys info */
	  if (btree_is_unique (thread_p, &btree_stats_p->btid))
	    {
	      max_unique_keys = MAX (max_unique_keys, btree_stats_p->keys);
	    }

	  OR_PUT_BTID (buf_p, &btree_stats_p->btid);
	  buf_p += OR_BTID_ALIGNED_SIZE;

	  /* defense for not gathered statistics */
	  btree_stats_p->leafs = MAX (1, btree_stats_p->leafs);
	  btree_stats_p->pages = MAX (1, btree_stats_p->pages);
	  btree_stats_p->height = MAX (1, btree_stats_p->height);

	  /* If the btree file has currently more pages than when we gathered
	     statistics, assume that all growth happen at the leaf level. If
	     the btree is smaller, we use the gathered statistics since the
	     btree may have an external file (unknown at this level) to keep
	     overflow keys. */
	  npages = file_get_numpages (thread_p, &btree_stats_p->btid.vfid);
	  if (npages > btree_stats_p->pages)
	    {
	      OR_PUT_INT (buf_p, (btree_stats_p->leafs +
				  (npages - btree_stats_p->pages)));
	      buf_p += OR_INT_SIZE;

	      OR_PUT_INT (buf_p, npages);
	      buf_p += OR_INT_SIZE;
	    }
	  else
	    {
	      OR_PUT_INT (buf_p, btree_stats_p->leafs);
	      buf_p += OR_INT_SIZE;

	      OR_PUT_INT (buf_p, btree_stats_p->pages);
	      buf_p += OR_INT_SIZE;
	    }

	  OR_PUT_INT (buf_p, btree_stats_p->height);
	  buf_p += OR_INT_SIZE;

	  OR_PUT_INT (buf_p, btree_stats_p->has_function);
	  buf_p += OR_INT_SIZE;

	  /* check and handle with estimation,
	   * since pkeys[] is not gathered before update stats
	   */
	  if (estimated_nobjs > 0)
	    {
	      /* is non-empty index */
	      btree_stats_p->keys = MAX (btree_stats_p->keys, 1);

	      /* If the estimated objects from heap manager is greater than
	       * the estimate when the statistics were gathered, assume that
	       * the difference is in distinct keys.
	       */
	      if (cls_info_p->tot_objects > 0
		  && estimated_nobjs > cls_info_p->tot_objects)
		{
		  btree_stats_p->keys +=
		    (estimated_nobjs - cls_info_p->tot_objects);
		}
	    }

	  OR_PUT_INT (buf_p, btree_stats_p->keys);
	  buf_p += OR_INT_SIZE;

	  assert_release (btree_stats_p->leafs >= 1);
	  assert_release (btree_stats_p->pages >= 1);
	  assert_release (btree_stats_p->height >= 1);
	  assert_release (btree_stats_p->keys >= 0);

	  buf_p = or_pack_domain (buf_p, btree_stats_p->key_type, 0, 0);

	  /* get full key size */
	  if (TP_DOMAIN_TYPE (btree_stats_p->key_type) == DB_TYPE_MIDXKEY)
	    {
	      key_size = tp_domain_size (btree_stats_p->key_type->setdomain);
	    }
	  else
	    {
	      key_size = 1;
	    }
	  assert (key_size >= btree_stats_p->pkeys_size);

	  assert (btree_stats_p->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	  for (k = 0; k < btree_stats_p->pkeys_size; k++)
	    {
	      /* check and handle with estimation,
	       * since pkeys[] is not gathered before update stats
	       */
	      if (estimated_nobjs > 0)
		{
		  /* is non-empty index */
		  btree_stats_p->pkeys[k] = MAX (btree_stats_p->pkeys[k], 1);
		}

	      if (k + 1 == key_size)
		{
		  /* this last pkeys must be equal to keys */
		  btree_stats_p->pkeys[k] = btree_stats_p->keys;
		}

	      OR_PUT_INT (buf_p, btree_stats_p->pkeys[k]);
	      buf_p += OR_INT_SIZE;
	    }
	}			/* for (j = 0, ...) */
    }

  OR_PUT_INT (buf_p, max_unique_keys);
  buf_p += OR_INT_SIZE;

  catalog_free_representation (disk_repr_p);
  catalog_free_class_info (cls_info_p);

  *length_p = CAST_STRLEN (buf_p - start_p);

  return start_p;
}

/*
 * qst_get_class_list () - Build the list of OIDs of classes
 *   return: NO_ERROR or error code
 *   key(in): next class OID to be added to the list
 *   val(in): data value associated with the class id on the ext. hash entry
 *   args(in): class list being built
 *
 * Note: This function builds the next node of the class id list. It is passed
 *       to the ehash_map function to be called once on each item kept on the
 *       extendible hashing structure used by the catalog manager.
 */
static int
stats_get_class_list (THREAD_ENTRY * thread_p, void *key, void *val,
		      void *args)
{
  CLASS_ID_LIST *class_id_item_p, **p;

  p = (CLASS_ID_LIST **) args;
  class_id_item_p = (CLASS_ID_LIST *) db_private_alloc (thread_p,
							sizeof
							(CLASS_ID_LIST));
  if (class_id_item_p == NULL)
    {
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  class_id_item_p->class_id.volid = ((OID *) key)->volid;
  class_id_item_p->class_id.pageid = ((OID *) key)->pageid;
  class_id_item_p->class_id.slotid = ((OID *) key)->slotid;
  class_id_item_p->next = *p;

  *p = class_id_item_p;
  return NO_ERROR;
}

/*
 * qst_free_class_list () - Frees the dynamic memory area used by the passed
 *                          linked list
 *   return: void
 *   class_id_list(in): list to be freed
 */
static void
stats_free_class_list (CLASS_ID_LIST * class_id_list_p)
{
  CLASS_ID_LIST *p, *next_p;

  if (class_id_list_p == NULL)
    {
      return;
    }

  for (p = class_id_list_p; p; p = next_p)
    {
      next_p = p->next;
      db_private_free_and_init (NULL, p);
    }

  class_id_list_p = NULL;
}

/*
 * qst_date_compar () -
 *   return:
 *   date1(in): First date value
 *   date2(in): Second date value
 *
 * Note: This function compares two date values and returns an integer less
 *       than, equal to, or greater than 0, if the first one is less than,
 *       equal to, or greater than the second one, respectively.
 */
static int
stats_compare_date (DB_DATE * date1_p, DB_DATE * date2_p)
{
  return (*date1_p - *date2_p);
}

/*
 * qst_time_compar () -
 *   return:
 *   time1(in): First time value
 *   time2(in): Second time value
 *
 * Note: This function compares two time values and returns an integer less
 *       than, equal to, or greater than 0, if the first one is less than,
 *       equal to, or greater than the second one, respectively.
 */
static int
stats_compare_time (DB_TIME * time1_p, DB_TIME * time2_p)
{
  return (int) (*time1_p - *time2_p);
}

/*
 * qst_utime_compar () -
 *   return:
 *   utime1(in): First utime value
 *   utime2(in): Second utime value
 *
 * Note: This function compares two utime values and returns an integer less
 *       than, equal to, or greater than 0, if the first one is less than,
 *       equal to, or greater than the second one, respectively.
 */
static int
stats_compare_utime (DB_UTIME * utime1_p, DB_UTIME * utime2_p)
{
  return (int) (*utime1_p - *utime2_p);
}

/*
 * stats_compare_datetime () -
 *   return:
 *   datetime1(in): First datetime value
 *   datetime2(in): Second datetime value
 *
 * Note: This function compares two datetime values and returns an integer less
 *       than, equal to, or greater than 0, if the first one is less than,
 *       equal to, or greater than the second one, respectively.
 */
static int
stats_compare_datetime (DB_DATETIME * datetime1_p, DB_DATETIME * datetime2_p)
{
  if (datetime1_p->date < datetime2_p->date)
    {
      return -1;
    }
  else if (datetime1_p->date > datetime2_p->date)
    {
      return 1;
    }
  else if (datetime1_p->time < datetime2_p->time)
    {
      return -1;
    }
  else if (datetime1_p->time > datetime2_p->time)
    {
      return 1;
    }
  else
    {
      return 0;
    }
}

/*
 * qst_money_compar () -
 *   return:
 *   mn1(in): First money value
 *   ,n2(in): Second money value
 *
 * Note: This function compares two money values and returns an integer less
 *       than, equal to, or greater than 0, if the first one is less than,
 *       equal to, or greater than the second one, respectively.
 */
static int
stats_compare_money (DB_MONETARY * money1_p, DB_MONETARY * money2_p)
{
  double comp_result = money1_p->amount - money2_p->amount;

  if (comp_result == 0.0)
    {
      return 0;
    }
  else if (comp_result < 0.0)
    {
      return -1;
    }
  else
    {
      return 1;
    }
}

/*
 * stats_get_time_stamp () - returns the current system time
 *   return: current system time
 */
unsigned int
stats_get_time_stamp (void)
{
  time_t tloc;

  return (unsigned int) time (&tloc);
}

/*
 * qst_data_compare () -
 *   return:
 *   data1(in):
 *   data2(in):
 *   type(in):
 */
static int
stats_compare_data (DB_DATA * data1_p, DB_DATA * data2_p, DB_TYPE type)
{
  int status = 0;

  switch (type)
    {
    case DB_TYPE_INTEGER:
      status = ((data1_p->i < data2_p->i)
		? DB_LT : ((data1_p->i > data2_p->i) ? DB_GT : DB_EQ));
      break;

    case DB_TYPE_BIGINT:
      status = ((data1_p->bigint < data2_p->bigint)
		? DB_LT : ((data1_p->bigint > data2_p->bigint) ? DB_GT :
			   DB_EQ));
      break;

    case DB_TYPE_SHORT:
      status = ((data1_p->sh < data2_p->sh)
		? DB_LT : ((data1_p->sh > data2_p->sh) ? DB_GT : DB_EQ));
      break;

    case DB_TYPE_FLOAT:
      status = ((data1_p->f < data2_p->f)
		? DB_LT : ((data1_p->f > data2_p->f) ? DB_GT : DB_EQ));
      break;

    case DB_TYPE_DOUBLE:
      status = ((data1_p->d < data2_p->d)
		? DB_LT : ((data1_p->d > data2_p->d) ? DB_GT : DB_EQ));
      break;

    case DB_TYPE_DATE:
      status = stats_compare_date (&data1_p->date, &data2_p->date);
      break;

    case DB_TYPE_TIME:
      status = stats_compare_time (&data1_p->time, &data2_p->time);
      break;

    case DB_TYPE_UTIME:
      status = stats_compare_utime (&data1_p->utime, &data2_p->utime);
      break;

    case DB_TYPE_DATETIME:
      status = stats_compare_datetime (&data1_p->datetime,
				       &data2_p->datetime);
      break;

    case DB_TYPE_MONETARY:
      status = stats_compare_money (&data1_p->money, &data2_p->money);
      break;

    default:
      status = 0;
      break;
    }
  return (status);
}

#if defined(CUBRID_DEBUG)
/*
 * stats_dump_class_stats () - Dumps the given statistics about a class
 *   return:
 *   class_stats(in): statistics to be printed
 *   fpp(in):
 */
void
stats_dump_class_statistics (CLASS_STATS * class_stats, FILE * fpp)
{
  int i, j, k;
  const char *prefix = "";
  time_t tloc;

  if (class_stats == NULL)
    {
      return;
    }

  fprintf (fpp, "\nCLASS STATISTICS\n");
  fprintf (fpp, "****************\n");
  tloc = (time_t) class_stats->time_stamp;
  fprintf (fpp, " Timestamp: %s", ctime (&tloc));
  fprintf (fpp, " Total Pages in Class Heap: %d\n",
	   class_stats->heap_num_pages);
  fprintf (fpp, " Total Objects: %d\n", class_stats->heap_num_objects);
  fprintf (fpp, " Number of attributes: %d\n", class_stats->n_attrs);

  for (i = 0; i < class_stats->n_attrs; i++)
    {
      fprintf (fpp, "\n Atrribute :\n");
      fprintf (fpp, "    id: %d\n", class_stats->attr_stats[i].id);
      fprintf (fpp, "    Type: ");

      switch (class_stats->attr_stats[i].type)
	{
	case DB_TYPE_INTEGER:
	  fprintf (fpp, "DB_TYPE_INTEGER \n");
	  break;

	case DB_TYPE_BIGINT:
	  fprintf (fpp, "DB_TYPE_BIGINT \n");
	  break;

	case DB_TYPE_SHORT:
	  fprintf (fpp, "DB_TYPE_SHORT \n");
	  break;

	case DB_TYPE_FLOAT:
	  fprintf (fpp, "DB_TYPE_FLOAT \n");
	  break;

	case DB_TYPE_DOUBLE:
	  fprintf (fpp, "DB_TYPE_DOUBLE \n");
	  break;

	case DB_TYPE_STRING:
	  fprintf (fpp, "DB_TYPE_STRING \n");
	  break;

	case DB_TYPE_OBJECT:
	  fprintf (fpp, "DB_TYPE_OBJECT \n");
	  break;

	case DB_TYPE_SET:
	  fprintf (fpp, "DB_TYPE_SET \n");
	  break;

	case DB_TYPE_MULTISET:
	  fprintf (fpp, "DB_TYPE_MULTISET \n");
	  break;

	case DB_TYPE_SEQUENCE:
	  fprintf (fpp, "DB_TYPE_SEQUENCE \n");
	  break;

	case DB_TYPE_TIME:
	  fprintf (fpp, "DB_TYPE_TIME \n");
	  break;

	case DB_TYPE_UTIME:
	  fprintf (fpp, "DB_TYPE_UTIME \n");
	  break;

	case DB_TYPE_DATETIME:
	  fprintf (fpp, "DB_TYPE_DATETIME \n");
	  break;

	case DB_TYPE_MONETARY:
	  fprintf (fpp, "DB_TYPE_MONETARY \n");
	  break;

	case DB_TYPE_DATE:
	  fprintf (fpp, "DB_TYPE_DATE \n");
	  break;

	case DB_TYPE_ELO:
	  fprintf (fpp, "DB_TYPE_ELO \n");
	  break;

	case DB_TYPE_BLOB:
	  fprintf (fpp, "DB_TYPE_BLOB \n");
	  break;

	case DB_TYPE_CLOB:
	  fprintf (fpp, "DB_TYPE_CLOB \n");
	  break;

	case DB_TYPE_VARIABLE:
	  fprintf (fpp, "DB_TYPE_VARIABLE  \n");
	  break;

	case DB_TYPE_SUB:
	  fprintf (fpp, "DB_TYPE_SUB \n");
	  break;

	case DB_TYPE_POINTER:
	  fprintf (fpp, "DB_TYPE_POINTER \n");
	  break;

	case DB_TYPE_NULL:
	  fprintf (fpp, "DB_TYPE_NULL \n");
	  break;

	case DB_TYPE_NUMERIC:
	  fprintf (fpp, "DB_TYPE_NUMERIC \n");
	  break;

	case DB_TYPE_BIT:
	  fprintf (fpp, "DB_TYPE_BIT \n");
	  break;

	case DB_TYPE_VARBIT:
	  fprintf (fpp, "DB_TYPE_VARBIT \n");
	  break;

	case DB_TYPE_CHAR:
	  fprintf (fpp, "DB_TYPE_CHAR \n");
	  break;

	case DB_TYPE_NCHAR:
	  fprintf (fpp, "DB_TYPE_NCHAR \n");
	  break;

	case DB_TYPE_VARNCHAR:
	  fprintf (fpp, "DB_TYPE_VARNCHAR \n");
	  break;

	default:
	  break;
	}

      fprintf (fpp, "    BTree statistics:\n");

      for (j = 0; j < class_stats->attr_stats[i].n_btstats; j++)
	{
	  BTREE_STATS *bt_statsp = &class_stats->attr_stats[i].bt_stats[j];
	  fprintf (fpp, "        BTID: { %d , %d }\n",
		   bt_statsp->btid.vfid.volid, bt_statsp->btid.vfid.fileid);
	  fprintf (fpp, "        Cardinality: %d (", bt_statsp->keys);

	  prefix = "";
	  assert (bt_statsp->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	  for (k = 0; k < bt_statsp->pkeys_size; k++)
	    {
	      fprintf (fpp, "%s%d", prefix, bt_statsp->pkeys[k]);
	      prefix = ",";
	    }

	  fprintf (fpp, ") ,");
	  fprintf (fpp, " Total Pages: %d , Leaf Pages: %d ,"
		   " Height: %d\n",
		   bt_statsp->pages, bt_statsp->leafs, bt_statsp->height);
	}
      fprintf (fpp, "\n");
    }

  fprintf (fpp, "\n\n");
}
#endif /* CUBRID_DEBUG */

/*
 * stats_update_partitioned_statistics () - compute statistics for a
 *						  partitioned class
 * return : error code or NO_ERROR
 * thread_p (in) :
 * class_id_p (in) : oid of the partitioned class
 * partitions (in) : oids of partitions
 * int partitions_count (in) : number of partitions
 * with_fullscan(in): true iff WITH FULLSCAN
 *
 * Note: Since, during plan generation we only have access to the partitioned
 * class, we have to keep an estimate of average statistics in this class. We
 * are using the average because we consider the case in which only one
 * partition is pruned to be the most common case.
 *
 * The average is computed as the mean (1 + coefficient of variation). We
 * use this coefficient to account for situations in which an index is not
 * balanced in the hierarchy (i.e: has lots of keys in some partitions but
 * very few keys in others). This type of distribution should be considered
 * worse than a balanced distribution since we don't know which partition
 * will be used in the query.
 */
static int
stats_update_partitioned_statistics (THREAD_ENTRY * thread_p,
				     OID * class_id_p, OID * partitions,
				     int partitions_count, bool with_fullscan)
{
  int i, j, k, btree_iter, m;
  int error = NO_ERROR;
  CLS_INFO *cls_info_p = NULL;
  CLS_INFO *subcls_info = NULL;
  DISK_REPR *disk_repr_p = NULL, *subcls_disk_rep = NULL;
  REPR_ID repr_id = NULL_REPRID, subcls_repr_id = NULL_REPRID;
  DISK_ATTR *disk_attr_p = NULL, *subcls_attr_p = NULL;
  BTREE_STATS *btree_stats_p = NULL;
  BTREE_STATS *computed_stats = NULL;
  int n_btrees = 0;
  PARTITION_STATS_ACUMULATOR *mean = NULL, *stddev = NULL;
  OR_CLASSREP *cls_rep = NULL;
  OR_CLASSREP *subcls_rep = NULL;
  int cls_idx_cache = 0, subcls_idx_cache = 0;

  assert_release (class_id_p != NULL);
  assert_release (partitions != NULL);
  assert_release (partitions_count > 0);

  for (i = 0; i < partitions_count; i++)
    {
      error =
	xstats_update_statistics (thread_p, &partitions[i], NULL,
				  with_fullscan);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}
    }

  cls_info_p = catalog_get_class_info (thread_p, class_id_p);
  if (cls_info_p == NULL)
    {
      error = er_errid ();
      goto cleanup;
    }

  error = catalog_get_last_representation_id (thread_p, class_id_p, &repr_id);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  cls_info_p->tot_pages = 0;
  cls_info_p->tot_objects = 0;

  disk_repr_p = catalog_get_representation (thread_p, class_id_p, repr_id);
  if (disk_repr_p == NULL)
    {
      error = er_errid ();
      goto cleanup;
    }
  /* partitions_count number of btree_stats we will need to use */
  n_btrees = 0;
  for (i = 0; i < disk_repr_p->n_fixed + disk_repr_p->n_variable; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  n_btrees++;
	}
    }

  if (n_btrees == 0)
    {
      /* We're not really interested in this */
      error = NO_ERROR;
      goto cleanup;
    }

  mean =
    (PARTITION_STATS_ACUMULATOR *) db_private_alloc (thread_p,
						     n_btrees *
						     sizeof
						     (PARTITION_STATS_ACUMULATOR));
  if (mean == NULL)
    {
      error = ER_FAILED;
      goto cleanup;
    }

  stddev =
    (PARTITION_STATS_ACUMULATOR *) db_private_alloc (thread_p,
						     n_btrees *
						     sizeof
						     (PARTITION_STATS_ACUMULATOR));
  if (stddev == NULL)
    {
      error = ER_FAILED;
      goto cleanup;
    }

  memset (mean, 0, n_btrees * sizeof (PARTITION_STATS_ACUMULATOR));
  memset (stddev, 0, n_btrees * sizeof (PARTITION_STATS_ACUMULATOR));

  /* initialize pkeys */
  btree_iter = 0;
  for (i = 0; i < disk_repr_p->n_fixed + disk_repr_p->n_variable; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  mean[btree_iter].pkeys_size = btree_stats_p->pkeys_size;
	  mean[btree_iter].pkeys =
	    (double *) db_private_alloc (thread_p,
					 mean[btree_iter].pkeys_size *
					 sizeof (double));
	  if (mean[btree_iter].pkeys == NULL)
	    {
	      error = ER_FAILED;
	      goto cleanup;
	    }
	  memset (mean[btree_iter].pkeys, 0,
		  mean[btree_iter].pkeys_size * sizeof (double));

	  stddev[btree_iter].pkeys_size = btree_stats_p->pkeys_size;
	  stddev[btree_iter].pkeys =
	    (double *) db_private_alloc (thread_p,
					 mean[btree_iter].pkeys_size *
					 sizeof (double));
	  if (stddev[btree_iter].pkeys == NULL)
	    {
	      error = ER_FAILED;
	      goto cleanup;
	    }
	  memset (stddev[btree_iter].pkeys, 0,
		  mean[btree_iter].pkeys_size * sizeof (double));
	  DB_MAKE_NULL (&btree_stats_p->unused_min_value);
	  DB_MAKE_NULL (&btree_stats_p->unused_max_value);
	  btree_iter++;
	}
    }
  /* Compute class statistics:
   * For each btree compute the mean, standard deviation and coefficient of
   * variation. The global statistics for each btree will be the
   * mean * (1 + cv). We do this so that the optimizer will pick the tree with
   * the lowest dispersion.
   * Since partitions and partitioned class have the same indexes but not
   * stored in the same order, we will use class representations to match
   * inherited indexes from partitions to the indexes in the partitioned class
   * class.
   */
  cls_rep = heap_classrepr_get (thread_p, class_id_p, NULL, 0, &cls_idx_cache,
				true);
  if (cls_rep == NULL)
    {
      error = ER_FAILED;
      goto cleanup;
    }

  for (i = 0; i < partitions_count; i++)
    {
      /* clean subclass loaded in previous iteration */
      if (subcls_info != NULL)
	{
	  catalog_free_class_info (subcls_info);
	  subcls_info = NULL;
	}
      if (subcls_disk_rep != NULL)
	{
	  catalog_free_representation (subcls_disk_rep);
	  subcls_disk_rep = NULL;
	}
      if (subcls_rep != NULL)
	{
	  heap_classrepr_free (subcls_rep, &subcls_idx_cache);
	  subcls_rep = NULL;
	}

      /* load new subclass */
      subcls_info = catalog_get_class_info (thread_p, &partitions[i]);
      if (subcls_info == NULL)
	{
	  error = er_errid ();
	  goto cleanup;
	}
      cls_info_p->tot_pages += subcls_info->tot_pages;
      cls_info_p->tot_objects += subcls_info->tot_objects;

      /* get disk repr for subclass */
      error = catalog_get_last_representation_id (thread_p, &partitions[i],
						  &subcls_repr_id);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      subcls_disk_rep = catalog_get_representation (thread_p, &partitions[i],
						    subcls_repr_id);
      if (subcls_disk_rep == NULL)
	{
	  error = er_errid ();
	  goto cleanup;
	}

      subcls_rep = heap_classrepr_get (thread_p, &partitions[i], NULL, 0,
				       &subcls_idx_cache, true);
      if (subcls_rep == NULL)
	{
	  error = ER_FAILED;
	  goto cleanup;
	}

      /* add partition information to the accumulators and also update
       * min/max values for partitioned class disk representation
       */
      btree_iter = 0;
      for (j = 0; j < subcls_disk_rep->n_fixed + subcls_disk_rep->n_variable;
	   j++)
	{
	  if (j < subcls_disk_rep->n_fixed)
	    {
	      subcls_attr_p = subcls_disk_rep->fixed + j;
	      disk_attr_p = disk_repr_p->fixed + j;
	    }
	  else
	    {
	      subcls_attr_p =
		subcls_disk_rep->variable + (j - subcls_disk_rep->n_fixed);
	      disk_attr_p =
		disk_repr_p->variable + (j - disk_repr_p->n_fixed);
	    }

	  /* check for partitions schema changes are not yet finished */
	  if (subcls_attr_p->id != disk_attr_p->id
	      || subcls_attr_p->n_btstats != disk_attr_p->n_btstats)
	    {
	      error = NO_ERROR;
	      goto cleanup;
	    }

	  assert_release (subcls_attr_p->id == disk_attr_p->id);
	  assert_release (subcls_attr_p->n_btstats == disk_attr_p->n_btstats);

	  for (k = 0, btree_stats_p = disk_attr_p->bt_stats;
	       k < disk_attr_p->n_btstats; k++, btree_stats_p++)
	    {
	      const BTREE_STATS *subcls_stats =
		stats_find_inherited_index_stats (cls_rep, subcls_rep,
						  subcls_attr_p,
						  &btree_stats_p->btid);
	      if (subcls_stats == NULL)
		{
		  error = ER_FAILED;
		  goto cleanup;
		}

	      mean[btree_iter].leafs += subcls_stats->leafs;

	      mean[btree_iter].pages += subcls_stats->pages;

	      mean[btree_iter].height += subcls_stats->height;

	      mean[btree_iter].keys += subcls_stats->keys;

	      assert (subcls_stats->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	      for (m = 0; m < subcls_stats->pkeys_size; m++)
		{
		  mean[btree_iter].pkeys[m] += subcls_stats->pkeys[m];
		}

	      btree_iter++;
	    }
	}
    }

  /* compute actual mean */
  for (btree_iter = 0; btree_iter < n_btrees; btree_iter++)
    {
      mean[btree_iter].leafs /= partitions_count;

      mean[btree_iter].pages /= partitions_count;

      mean[btree_iter].height /= partitions_count;

      mean[btree_iter].keys /= partitions_count;

      assert (mean[btree_iter].pkeys_size <= BTREE_STATS_PKEYS_NUM);
      for (m = 0; m < mean[btree_iter].pkeys_size; m++)
	{
	  mean[btree_iter].pkeys[m] /= partitions_count;
	}
    }

  for (i = 0; i < partitions_count; i++)
    {
      /* clean subclass loaded in previous iteration */
      if (subcls_disk_rep != NULL)
	{
	  catalog_free_representation (subcls_disk_rep);
	  subcls_disk_rep = NULL;
	}
      if (subcls_rep != NULL)
	{
	  heap_classrepr_free (subcls_rep, &subcls_idx_cache);
	  subcls_rep = NULL;
	}

      /* get disk repr for subclass */
      error = catalog_get_last_representation_id (thread_p, &partitions[i],
						  &subcls_repr_id);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      subcls_disk_rep = catalog_get_representation (thread_p, &partitions[i],
						    subcls_repr_id);
      if (subcls_disk_rep == NULL)
	{
	  error = er_errid ();
	  goto cleanup;
	}

      subcls_rep = heap_classrepr_get (thread_p, &partitions[i], NULL, 0,
				       &subcls_idx_cache, true);
      if (subcls_rep == NULL)
	{
	  error = ER_FAILED;
	  goto cleanup;
	}

      /* add partition information to the accumulators */
      btree_iter = 0;
      for (j = 0; j < subcls_disk_rep->n_fixed + subcls_disk_rep->n_variable;
	   j++)
	{
	  if (j < subcls_disk_rep->n_fixed)
	    {
	      subcls_attr_p = subcls_disk_rep->fixed + j;
	      disk_attr_p = disk_repr_p->fixed + j;
	    }
	  else
	    {
	      subcls_attr_p =
		subcls_disk_rep->variable + (j - subcls_disk_rep->n_fixed);
	      disk_attr_p =
		disk_repr_p->variable + (j - disk_repr_p->n_fixed);
	    }

	  /* check for partitions schema changes are not yet finished */
	  if (subcls_attr_p->id != disk_attr_p->id
	      || subcls_attr_p->n_btstats != disk_attr_p->n_btstats)
	    {
	      assert (false);	/* is impossible */
	      error = NO_ERROR;
	      goto cleanup;
	    }

	  assert_release (subcls_attr_p->id == disk_attr_p->id);
	  assert_release (subcls_attr_p->n_btstats == disk_attr_p->n_btstats);

	  for (k = 0, btree_stats_p = disk_attr_p->bt_stats;
	       k < disk_attr_p->n_btstats; k++, btree_stats_p++)
	    {
	      const BTREE_STATS *subcls_stats =
		stats_find_inherited_index_stats (cls_rep, subcls_rep,
						  subcls_attr_p,
						  &btree_stats_p->btid);
	      if (subcls_stats == NULL)
		{
		  error = ER_FAILED;
		  goto cleanup;
		}

	      stddev[btree_iter].leafs +=
		SQUARE (btree_stats_p->leafs - mean[btree_iter].leafs);

	      stddev[btree_iter].pages +=
		SQUARE (subcls_stats->pages - mean[btree_iter].pages);

	      stddev[btree_iter].height +=
		SQUARE (subcls_stats->height - mean[btree_iter].height);

	      stddev[btree_iter].keys +=
		SQUARE (subcls_stats->keys - mean[btree_iter].keys);

	      assert (subcls_stats->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	      for (m = 0; m < subcls_stats->pkeys_size; m++)
		{
		  stddev[btree_iter].pkeys[m] +=
		    SQUARE (subcls_stats->pkeys[m] -
			    mean[btree_iter].pkeys[m]);
		}

	      btree_iter++;
	    }
	}
    }

  for (btree_iter = 0; btree_iter < n_btrees; btree_iter++)
    {
      stddev[btree_iter].leafs =
	sqrt (stddev[btree_iter].leafs / partitions_count);

      stddev[btree_iter].pages =
	sqrt (stddev[btree_iter].pages / partitions_count);

      stddev[btree_iter].height =
	sqrt (stddev[btree_iter].height / partitions_count);

      stddev[btree_iter].keys =
	sqrt (stddev[btree_iter].keys / partitions_count);

      assert (mean[btree_iter].pkeys_size <= BTREE_STATS_PKEYS_NUM);
      for (m = 0; m < mean[btree_iter].pkeys_size; m++)
	{
	  stddev[btree_iter].pkeys[m] =
	    sqrt (stddev[btree_iter].pkeys[m] / partitions_count);
	}
    }

  /* compute new statistics */
  btree_iter = 0;
  for (i = 0; i < disk_repr_p->n_fixed + disk_repr_p->n_variable; i++)
    {
      if (i < disk_repr_p->n_fixed)
	{
	  disk_attr_p = disk_repr_p->fixed + i;
	}
      else
	{
	  disk_attr_p = disk_repr_p->variable + (i - disk_repr_p->n_fixed);
	}

      for (j = 0, btree_stats_p = disk_attr_p->bt_stats;
	   j < disk_attr_p->n_btstats; j++, btree_stats_p++)
	{
	  if (mean[btree_iter].leafs != 0)
	    {
	      btree_stats_p->leafs =
		(int) (mean[btree_iter].leafs *
		       (1 +
			stddev[btree_iter].leafs / mean[btree_iter].leafs));
	    }
	  if (mean[btree_iter].pages != 0)
	    {
	      btree_stats_p->pages =
		(int) (mean[btree_iter].pages *
		       (1 +
			stddev[btree_iter].pages / mean[btree_iter].pages));
	    }
	  if (mean[btree_iter].height != 0)
	    {
	      btree_stats_p->height =
		(int) (mean[btree_iter].height *
		       (1 +
			stddev[btree_iter].height / mean[btree_iter].height));
	    }
	  if (mean[btree_iter].keys != 0)
	    {
	      btree_stats_p->keys =
		(int) (mean[btree_iter].keys *
		       (1 + stddev[btree_iter].keys / mean[btree_iter].keys));
	    }

	  assert (mean[btree_iter].pkeys_size <= BTREE_STATS_PKEYS_NUM);
	  for (m = 0; m < mean[btree_iter].pkeys_size; m++)
	    {
	      if (mean[btree_iter].pkeys[m] != 0)
		{
		  btree_stats_p->pkeys[m] =
		    (int) (mean[btree_iter].pkeys[m] * (1 +
							stddev[btree_iter].
							pkeys[m] /
							mean[btree_iter].
							pkeys[m]));
		}
	    }
	  btree_iter++;
	}
    }

  /* replace the current disk representation structure/information in the
     catalog with the newly computed statistics */
  error = catalog_add_representation (thread_p, class_id_p, repr_id,
				      disk_repr_p);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  cls_info_p->time_stamp = stats_get_time_stamp ();

  error = catalog_add_class_info (thread_p, class_id_p, cls_info_p);

cleanup:
  if (cls_rep != NULL)
    {
      heap_classrepr_free (cls_rep, &cls_idx_cache);
    }
  if (subcls_rep != NULL)
    {
      heap_classrepr_free (subcls_rep, &subcls_idx_cache);
    }
  if (mean != NULL)
    {
      for (i = 0; i < n_btrees; i++)
	{
	  if (mean[i].pkeys != NULL)
	    {
	      db_private_free (thread_p, mean[i].pkeys);
	    }
	}
      db_private_free (thread_p, mean);
    }
  if (stddev != NULL)
    {
      for (i = 0; i < n_btrees; i++)
	{
	  if (stddev[i].pkeys != NULL)
	    {
	      db_private_free (thread_p, stddev[i].pkeys);
	    }
	}
      db_private_free (thread_p, stddev);
    }
  if (subcls_info)
    {
      catalog_free_class_info (subcls_info);
    }
  if (subcls_disk_rep != NULL)
    {
      catalog_free_representation (subcls_disk_rep);
    }
  if (cls_info_p != NULL)
    {
      catalog_free_class_info (cls_info_p);
    }
  if (disk_repr_p != NULL)
    {
      catalog_free_representation (disk_repr_p);
    }

  return error;
}

/*
 * stats_find_inherited_index_stats () - find the btree statistics
 *					 corresponding to an index in a
 *					 subclass
 * return : btree statistics
 * cls_rep (in)	   : superclass representation
 * subcls_rep (in) : subclass representation
 * subcls_attr (in): subclass attribute representation which contains the
 *		     statistics
 * cls_btid (in)   : BTID of the index in the superclass
 */
const BTREE_STATS *
stats_find_inherited_index_stats (OR_CLASSREP * cls_rep,
				  OR_CLASSREP * subcls_rep,
				  DISK_ATTR * subcls_attr, BTID * cls_btid)
{
  int i;
  const char *cls_btname = NULL;
  const BTID *subcls_btid = NULL;
  BTREE_STATS *btstats = NULL;

  /* find the index representation in the superclass */
  for (i = 0; i < cls_rep->n_indexes; i++)
    {
      if (BTID_IS_EQUAL (&cls_rep->indexes[i].btid, cls_btid))
	{
	  cls_btname = cls_rep->indexes[i].btname;
	  break;
	}
    }

  if (cls_btname == NULL)
    {
      assert (false);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return NULL;
    }

  /* find the inherited index in the subclass */
  for (i = 0; i < subcls_rep->n_indexes; i++)
    {
      if (strcasecmp (cls_btname, subcls_rep->indexes[i].btname) == 0)
	{
	  subcls_btid = &subcls_rep->indexes[i].btid;
	  break;
	}
    }

  if (subcls_btid == NULL)
    {
      assert (false);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return NULL;
    }

  for (i = 0, btstats = subcls_attr->bt_stats; i < subcls_attr->n_btstats;
       i++, btstats++)
    {
      if (BTID_IS_EQUAL (subcls_btid, &btstats->btid))
	{
	  return btstats;
	}
    }

  assert (false);
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
  return NULL;
}
