/*
 * Copyright © 2010 Codethink Limited
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Ryan Lortie <desrt@desrt.ca>
 */

#include "dconf-changeset.h"
#include "dconf-paths.h"

#include <string.h>
#include <stdlib.h>

/**
 * SECTION:changeset
 * @title: DConfChangeset
 * @Short_description: A set of changes to a dconf database
 *
 * #DConfChangeset represents a set of changes that can be made to a
 * dconf database.  Currently supported operations are writing new
 * values to keys and resetting keys and dirs.
 *
 * Create the changeset with dconf_changeset_new() and populate it with
 * dconf_changeset_set().  Submit it to dconf with
 * dconf_client_change_fast() or dconf_client_change_sync().
 * dconf_changeset_new_write() is a convenience constructor for the
 * common case of writing or resetting a single value.
 **/

/**
 * DConfChangeset:
 *
 * This is a reference counted opaque structure type.  It is not a
 * #GObject.
 *
 * Use dconf_changeset_ref() and dconf_changeset_unref() to manipuate
 * references.
 **/

struct _DConfChangeset
{
  GHashTable *table;
  gint ref_count;

  gchar *prefix;
  const gchar **paths;
  GVariant **values;
};

static void
unref_gvariant0 (gpointer data)
{
  if (data)
    g_variant_unref (data);
}

/**
 * dconf_changeset_new:
 *
 * Creates a new, empty, #DConfChangeset.
 *
 * Returns: the new #DConfChangeset.
 **/
DConfChangeset *
dconf_changeset_new (void)
{
  DConfChangeset *changeset;

  changeset = g_slice_new0 (DConfChangeset);
  changeset->table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, unref_gvariant0);
  changeset->ref_count = 1;

  return changeset;
}

/**
 * dconf_changeset_unref:
 * @changeset: a #DConfChangeset
 *
 * Releases a #DConfChangeset reference.
 **/
void
dconf_changeset_unref (DConfChangeset *changeset)
{
  if (g_atomic_int_dec_and_test (&changeset->ref_count))
    {
      g_free (changeset->prefix);
      g_free (changeset->paths);
      g_free (changeset->values);

      g_hash_table_unref (changeset->table);

      g_slice_free (DConfChangeset, changeset);
    }
}

/**
 * dconf_changeset_ref:
 * @changeset: a #DConfChangeset
 *
 * Increases the reference count on @changeset
 *
 * Returns: @changeset
 **/
DConfChangeset *
dconf_changeset_ref (DConfChangeset *changeset)
{
  g_atomic_int_inc (&changeset->ref_count);

  return changeset;
}

/**
 * dconf_changeset_set:
 * @changeset: a #DConfChangeset
 * @path: a path to modify
 * @value: the value for the key, or %NULL to reset
 *
 * Adds an operation to modify @path to a #DConfChangeset.
 *
 * @path may either be a key or a dir.  If it is a key then @value may
 * be a #GVariant, or %NULL (to set or reset the key).
 *
 * If @path is a dir then this must be a reset operation: @value must be
 * %NULL.  It is not permitted to assign a #GVariant value to a dir.
 **/
void
dconf_changeset_set (DConfChangeset *changeset,
                     const gchar    *path,
                     GVariant       *value)
{
  g_return_if_fail (changeset->prefix == NULL);
  g_return_if_fail (dconf_is_path (path, NULL));

  /* Check if we are performing a path reset */
  if (g_str_has_suffix (path, "/"))
    {
      GHashTableIter iter;
      gpointer key;

      g_return_if_fail (value == NULL);

      /* When we reset a path we must also reset all keys within that
       * path.
       */
      g_hash_table_iter_init (&iter, changeset->table);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        if (g_str_has_prefix (key, path))
          g_hash_table_iter_remove (&iter);

      /* Record the reset itself. */
      g_hash_table_insert (changeset->table, g_strdup (path), NULL);
    }

  /* else, just a normal value write or reset */
  else
    g_hash_table_insert (changeset->table, g_strdup (path), value ? g_variant_ref_sink (value) : NULL);
}

/**
 * dconf_changeset_get:
 * @changeset: a #DConfChangeset
 * @key: the key to check
 * @value: a return location for the value, or %NULL
 *
 * Checks if a #DConfChangeset has an outstanding request to change
 * the value of the given @key.
 *
 * If the change doesn't involve @key then %FALSE is returned and the
 * @value is unmodified.
 *
 * If the change modifies @key then @value is set either to the value
 * for that key, or %NULL in the case that the key is being reset by the
 * request.
 *
 * Returns: %TRUE if the key is being modified by the change
 */
gboolean
dconf_changeset_get (DConfChangeset  *changeset,
                     const gchar     *key,
                     GVariant       **value)
{
  gpointer tmp;

  if (!g_hash_table_lookup_extended (changeset->table, key, NULL, &tmp))
    return FALSE;

  if (value)
    *value = tmp ? g_variant_ref (tmp) : NULL;

  return TRUE;
}

/**
 * dconf_changeset_is_similar_to:
 * @changeset: a #DConfChangeset
 * @other: another #DConfChangeset
 *
 * Checks if @changeset is similar to @other.
 *
 * Two changes are considered similar if they write to the exact same
 * set of keys.  The values written are not considered.
 *
 * This check is used to prevent building up a queue of repeated writes
 * of the same keys.  This is often seen when an application writes to a
 * key on every move of a slider or an application window.
 *
 * Strictly speaking, a write resettings all of "/a/" after a write
 * containing "/a/b" could cause the later to be removed from the queue,
 * but this situation is difficult to detect and is expected to be
 * extremely rare.
 *
 * Returns: %TRUE if the changes are similar
 **/
gboolean
dconf_changeset_is_similar_to (DConfChangeset *changeset,
                               DConfChangeset *other)
{
  GHashTableIter iter;
  gpointer key;

  if (g_hash_table_size (changeset->table) != g_hash_table_size (other->table))
    return FALSE;

  g_hash_table_iter_init (&iter, changeset->table);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (other->table, key))
      return FALSE;

  return TRUE;
}

/**
 * DConfChangesetPredicate:
 * @path: a path, as per dconf_is_path()
 * @value: a #GVariant, or %NULL
 * @user_data: user data pointer
 *
 * Callback function type for predicates over items in a
 * #DConfChangeset.
 *
 * Use with dconf_changeset_all().
 *
 * Returns: %TRUE if the predicate is met for the given @path and @value
 **/

/**
 * dconf_changeset_all:
 * @changeset: a #DConfChangeset
 * @predicate: a #DConfChangesetPredicate
 * @user_data: user data to pass to @predicate
 *
 * Checks if all changes in the changeset satisfy @predicate.
 *
 * @predicate is called on each item in the changeset, in turn, until it
 * returns %FALSE.
 *
 * If @preciate returns %FALSE for any item, this function returns
 * %FALSE.  If not (including the case of no items) then this function
 * returns %TRUE.
 *
 * Returns: %TRUE if all items in @changeset satisfy @predicate
 */
gboolean
dconf_changeset_all (DConfChangeset          *changeset,
                     DConfChangesetPredicate  predicate,
                     gpointer                 user_data)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, changeset->table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!(* predicate) (key, value, user_data))
      return FALSE;

  return TRUE;
}

static gint
dconf_changeset_string_ptr_compare (gconstpointer a_p,
                                    gconstpointer b_p)
{
  const gchar * const *a = a_p;
  const gchar * const *b = b_p;

  return strcmp (*a, *b);
}

static void
dconf_changeset_build_description (DConfChangeset *changeset)
{
  gsize prefix_length;
  gint n_items;

  n_items = g_hash_table_size (changeset->table);

  /* If there are no items then what is there to describe? */
  if (n_items == 0)
    return;

  /* We do three separate passes.  This might take a bit longer than
   * doing it all at once but it keeps the complexity down.
   *
   * First, we iterate the table in order to determine the common
   * prefix.
   *
   * Next, we iterate the table again to pull the strings out excluding
   * the leading prefix.
   *
   * We sort the list of paths at this point because the rebuilder
   * requires a sorted list.
   *
   * Finally, we iterate over the sorted list and use the normal
   * hashtable lookup in order to populate the values array in the same
   * order.
   *
   * Doing it this way avoids the complication of trying to sort two
   * arrays (keys and values) at the same time.
   */

  /* Pass 1: determine the common prefix. */
  {
    GHashTableIter iter;
    const gchar *first;
    gboolean have_one;
    gpointer key;

    g_hash_table_iter_init (&iter, changeset->table);

    /* We checked above that we have at least one item. */
    have_one = g_hash_table_iter_next (&iter, &key, NULL);
    g_assert (have_one);

    prefix_length = strlen (key);
    first = key;

    /* Consider the remaining items to find the common prefix */
    while (g_hash_table_iter_next (&iter, &key, NULL))
      {
        const gchar *this = key;
        gint i;

        for (i = 0; i < prefix_length; i++)
          if (first[i] != this[i])
            {
              prefix_length = i;
              break;
            }
      }

    /* We must surely always have a common prefix of '/' */
    g_assert (prefix_length > 0);
    g_assert (first[0] == '/');

    /* We may find that "/a/ab" and "/a/ac" have a common prefix of
     * "/a/a" but really we want to trim that back to "/a/".
     *
     * If there is only one item, leave it alone.
     */
    if (n_items > 1)
      {
        while (first[prefix_length - 1] != '/')
          prefix_length--;
      }

    changeset->prefix = g_strndup (first, prefix_length);
  }

  /* Pass 2: collect the list of keys, dropping the prefix */
  {
    GHashTableIter iter;
    gpointer key;
    gint i = 0;

    changeset->paths = g_new (const gchar *, n_items + 1);

    g_hash_table_iter_init (&iter, changeset->table);
    while (g_hash_table_iter_next (&iter, &key, NULL))
      {
        const gchar *path = key;

        changeset->paths[i++] = path + prefix_length;
      }
    changeset->paths[i] = NULL;
    g_assert (i == n_items);

    /* Sort the list of keys */
    qsort (changeset->paths, n_items, sizeof (const gchar *), dconf_changeset_string_ptr_compare);
  }

  /* Pass 3: collect the list of values */
  {
    gint i;

    changeset->values = g_new (GVariant *, n_items);

    for (i = 0; i < n_items; i++)
      /* We dropped the prefix when collecting the array.
       * Bring it back temporarily, for the lookup.
       */
      changeset->values[i] = g_hash_table_lookup (changeset->table, changeset->paths[i] - prefix_length);
  }
}

/**
 * dconf_changeset_describe:
 * @changeset: a #DConfChangeset
 * @prefix: the prefix under which changes have been requested
 * @paths: the list of paths changed, relative to @prefix
 * @values: the list of values changed
 *
 * Describes @changeset.
 *
 * @prefix and @paths are presented in the same way as they are for the
 * DConfClient::changed signal.  @values is an array of the same length
 * as @paths.  For each key described by an element in @paths, @values
 * will contain either a #GVariant (the requested new value of that key)
 * or %NULL (to reset a reset).
 *
 * The @paths array is returned in an order such that dir will always
 * come before keys contained within those dirs.
 *
 * Returns: the number of changes (the length of @changes and @values).
 **/
guint
dconf_changeset_describe (DConfChangeset       *changeset,
                          const gchar         **prefix,
                          const gchar * const **paths,
                          GVariant * const    **values)
{
  gint n_items;

  n_items = g_hash_table_size (changeset->table);

  if (n_items && !changeset->prefix)
    dconf_changeset_build_description (changeset);

  if (prefix)
    *prefix = changeset->prefix;

  if (paths)
    *paths = changeset->paths;

  if (values)
    *values = changeset->values;

  return n_items;
}

/**
 * dconf_changeset_serialise:
 * @changeset: a #DConfChangeset
 *
 * Serialises a #DConfChangeset.
 *
 * The returned value has no particular format and should only be passed
 * to dconf_changeset_deserialise().
 *
 * Returns: a floating #GVariant
 **/
GVariant *
dconf_changeset_serialise (DConfChangeset *changeset)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key, value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{smv}"));

  g_hash_table_iter_init (&iter, changeset->table);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_variant_builder_add (&builder, "{smv}", key, value);

  return g_variant_builder_end (&builder);
}

/**
 * dconf_changeset_deserialise:
 * @serialised: a #GVariant from dconf_changeset_serialise()
 *
 * Creates a #DConfChangeset according to a serialised description
 * returned from an earlier call to dconf_changeset_serialise().
 *
 * @serialised has no particular format -- you should only pass a value
 * that reasulted from an earlier serialise operation.
 *
 * This call never fails, even if @serialised is not in the correct
 * format.  Improperly-formatted parts are simply ignored.
 *
 * Returns: a new #DConfChangeset
 **/
DConfChangeset *
dconf_changeset_deserialise (GVariant *serialised)
{
  DConfChangeset *changeset;
  GVariantIter iter;
  const gchar *key;
  GVariant *value;

  changeset = dconf_changeset_new ();
  g_variant_iter_init (&iter, serialised);
  while (g_variant_iter_loop (&iter, "{&smv}", &key, &value))
    {
      /* If value is NULL: we may be resetting a key or a dir (a path).
       * If value is non-NULL: we must be setting a key.
       *
       * ie: it is not possible to set a value to a directory.
       *
       * If we get an invalid case, just fall through and ignore it.
       */
      if (value == NULL)
        {
          if (dconf_is_path (key, NULL))
            g_hash_table_insert (changeset->table, g_strdup (key), NULL);
        }
      else
        {
          if (dconf_is_key (key, NULL))
            g_hash_table_insert (changeset->table, g_strdup (key), g_variant_ref (value));
        }
    }

  return changeset;
}

/**
 * dconf_changeset_new_write:
 * @path: a dconf path
 * @value: a #GVariant, or %NULL
 *
 * Creates a new #DConfChangeset with one change.  This is equivalent to
 * calling dconf_changeset_new() and then dconf_changeset_set() with
 * @path and @value.
 *
 * Returns: a new #DConfChangeset
 **/
DConfChangeset *
dconf_changeset_new_write (const gchar *path,
                           GVariant    *value)
{
  DConfChangeset *changeset;

  changeset = dconf_changeset_new ();
  dconf_changeset_set (changeset, path, value);

  return changeset;
}