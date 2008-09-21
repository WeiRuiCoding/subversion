/*
 * revision_status.c: report the revision range and status of a working copy
 *
 * ====================================================================
 * Copyright (c) 2003-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include "svn_wc.h"

#include "svn_private_config.h"


/* A baton for analyze_status(). */
struct status_baton
{
  svn_wc_revision_status_t *result;           /* where to put the result */
  svn_boolean_t committed;           /* examine last committed revisions */
  const char *wc_path;               /* path whose URL we're looking for */
  const char *wc_url;    /* URL for the path whose URL we're looking for */
  apr_pool_t *pool;         /* pool in which to store alloc-needy things */
};

/* A walker_func_t callback function for analyzing WC status. */
static svn_error_t *
analyze_status(const char *path,
               void *baton,
               apr_pool_t *scratch_pool)
{
  struct status_baton *sb = baton;
  svn_revnum_t revision;
  const char *url;
  svn_wc__db_status_t status;
  svn_boolean_t text_mod;
  svn_boolean_t props_mod;
  svn_revnum_t original_rev;

  /* ### if sb->committed, then we need to read last-changed information
     ### from the base. need some API updates in wc_db.h for that. */

  SVN_ERR(svn_wc__db_read_info(NULL, &revision, &url, NULL, NULL, NULL,
                               &status, &text_mod, &props_mod, NULL, NULL,
                               &original_rev,
                               sb->db, path, scratch_pool, scratch_pool));

  sb->result->modified |= text_mod | props_mod;

  /* Added files have a revision of SVN_INVALID_REVNUM */
  if (revision == SVN_INVALID_REVNUM)
    {
      /* If it was copied or moved, then we'll use the revnum of the
         original node (could be SVN_INVALID_REVNUM if not copied/moved). */
      revision = original_rev;
    }

  if (sb->result->min_rev == SVN_INVALID_REVNUM
      || revision < sb->result->min_rev)
    sb->result->min_rev = revision;

  if (sb->result->max_rev == SVN_INVALID_REVNUM
      || revision > sb->result->max_rev)
    sb->result->max_rev = revision;

#if 0
  /* ### need to figure out how to read/store this data */
  sb->result->switched |= status->switched;
  sb->result->sparse_checkout |= (status->entry->depth != svn_depth_infinity);
#endif

  if (sb->wc_path
      && sb->wc_url == NULL
      && strcmp(path, sb->wc_path) == 0)
    sb->wc_url = apr_pstrdup(sb->pool, url);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_revision_status(svn_wc_revision_status_t **result_p,
                       const char *wc_path,
                       const char *trail_url,
                       svn_boolean_t committed,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *pool)
{
  struct status_baton sb;
  const char *target;
  svn_wc_adm_access_t *anchor_access, *target_access;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  svn_revnum_t edit_revision;
  svn_wc_revision_status_t *result = apr_palloc(pool, sizeof(**result_p));
  *result_p = result;

  /* set result as nil */
  result->min_rev  = SVN_INVALID_REVNUM;
  result->max_rev  = SVN_INVALID_REVNUM;
  result->switched = FALSE;
  result->modified = FALSE;
  result->sparse_checkout = FALSE;

  /* initialize walking baton */
  sb.result = result;
  sb.committed = committed;
  sb.wc_path = wc_path;
  sb.wc_url = NULL;
  sb.pool = pool;

  /* ### need to put cancel information into the baton, and then check
     ### that so we can exit from the walker. */

  /* ### pool as scratch_pool? we should probably update our signature */
  SVN_ERR(generic_walker(db, wc_path, walker_mode_working,
                         analyze_status, &sb, pool));

#if 0
  SVN_ERR(svn_wc_adm_open_anchor(&anchor_access, &target_access, &target,
                                 wc_path, FALSE, -1,
                                 cancel_func, cancel_baton,
                                 pool));

  SVN_ERR(svn_wc_get_status_editor3(&editor, &edit_baton, NULL,
                                    &edit_revision, anchor_access, target,
                                    svn_depth_infinity,
                                    TRUE  /* get_all */,
                                    FALSE /* no_ignore */,
                                    NULL  /* ignore_patterns */,
                                    analyze_status, &sb,
                                    cancel_func, cancel_baton,
                                    NULL  /* traversal_info */,
                                    pool));

  SVN_ERR(editor->close_edit(edit_baton, pool));

  SVN_ERR(svn_wc_adm_close(anchor_access));
#endif

  if ((! result->switched) && (trail_url != NULL))
    {
      /* If the trailing part of the URL of the working copy directory
         does not match the given trailing URL then the whole working
         copy is switched. */
      if (! sb.wc_url)
        {
          result->switched = TRUE;
        }
      else
        {
          apr_size_t len1 = strlen(trail_url);
          apr_size_t len2 = strlen(sb.wc_url);
          if ((len1 > len2) || strcmp(sb.wc_url + len2 - len1, trail_url))
            result->switched = TRUE;
        }
    }

  return SVN_NO_ERROR;
}
