/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * animation.c
 * Copyright (C) 2016 Jehan <jehan@gimp.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <libgimp/gimp.h>
#include <libgimp/stdplugins-intl.h>

#include "animation-utils.h"

#include "animation.h"
#include "animation-camera.h"
#include "animation-celanimation.h"

typedef struct _AnimationCelAnimationPrivate AnimationCelAnimationPrivate;

typedef struct
{
  gchar *title;
  /* List of list of layers identified by tattoos. */
  GList *frames;
}
Track;

struct _AnimationCelAnimationPrivate
{
  /* The number of frames. */
  gint             duration;
  gint             onion_skins;

  /* Panel comments. */
  GList           *comments;

  /* List of tracks/levels.
   * The background is a special-named track, always present
   * and first.
   * There is always at least 1 additional track. */
  GList           *tracks;

  /* The globale camera. */
  AnimationCamera *camera;
};

typedef enum
{
  START_STATE,
  ANIMATION_STATE,
  PLAYBACK_STATE,
  SEQUENCE_STATE,
  FRAME_STATE,
  LAYER_STATE,
  CAMERA_STATE,
  KEYFRAME_STATE,
  COMMENTS_STATE,
  COMMENT_STATE,
  END_STATE
} AnimationParseState;

typedef struct
{
  Animation           *animation;
  AnimationParseState  state;

  Track               *track;
  gint                 frame_position;
  gint                 frame_duration;
} ParseStatus;

#define GET_PRIVATE(animation) \
        G_TYPE_INSTANCE_GET_PRIVATE (animation, \
                                     ANIMATION_TYPE_CEL_ANIMATION, \
                                     AnimationCelAnimationPrivate)

static void         animation_cel_animation_constructed       (GObject      *object);
static void         animation_cel_animation_finalize          (GObject      *object);

/* Virtual methods */

static gint         animation_cel_animation_get_duration      (Animation    *animation);

static gchar      * animation_cel_animation_get_frame_hash    (Animation    *animation,
                                                               gint          position);
static GeglBuffer * animation_cel_animation_create_frame      (Animation    *animation,
                                                               GObject      *renderer,
                                                               gint          position,
                                                               gdouble       proxy_ratio);

static void         animation_cel_animation_reset_defaults    (Animation    *animation);
static gchar      * animation_cel_animation_serialize         (Animation    *animation,
                                                               const gchar  *playback_xml);
static gboolean     animation_cel_animation_deserialize       (Animation    *animation,
                                                               const gchar  *xml,
                                                               GError      **error);

static void         animation_cel_animation_update_paint_view (Animation    *animation,
                                                               gint          position);

/* XML parsing */

static void      animation_cel_animation_start_element     (GMarkupParseContext *context,
                                                            const gchar         *element_name,
                                                            const gchar        **attribute_names,
                                                            const gchar        **attribute_values,
                                                            gpointer             user_data,
                                                            GError             **error);
static void      animation_cel_animation_end_element       (GMarkupParseContext *context,
                                                            const gchar         *element_name,
                                                            gpointer             user_data,
                                                            GError             **error);

static void      animation_cel_animation_text              (GMarkupParseContext  *context,
                                                            const gchar          *text,
                                                            gsize                 text_len,
                                                            gpointer              user_data,
                                                            GError              **error);

/* Signal handling */

static void      on_camera_offsets_changed                 (AnimationCamera        *camera,
                                                            gint                    position,
                                                            gint                    duration,
                                                            AnimationCelAnimation  *animation);
/* Utils */

static void      animation_cel_animation_cleanup           (AnimationCelAnimation  *animation);
static void      animation_cel_animation_clean_track       (Track                   *track);
static gchar *   animation_cel_animation_get_hash          (AnimationCelAnimation  *animation,
                                                            gint                    position,
                                                            gboolean                layers_only);


G_DEFINE_TYPE (AnimationCelAnimation, animation_cel_animation, ANIMATION_TYPE_ANIMATION)

#define parent_class animation_cel_animation_parent_class

static void
animation_cel_animation_class_init (AnimationCelAnimationClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  AnimationClass *anim_class   = ANIMATION_CLASS (klass);

  object_class->constructed  = animation_cel_animation_constructed;
  object_class->finalize     = animation_cel_animation_finalize;

  anim_class->get_duration   = animation_cel_animation_get_duration;

  anim_class->get_frame_hash = animation_cel_animation_get_frame_hash;
  anim_class->create_frame   = animation_cel_animation_create_frame;

  anim_class->reset_defaults = animation_cel_animation_reset_defaults;
  anim_class->serialize      = animation_cel_animation_serialize;
  anim_class->deserialize    = animation_cel_animation_deserialize;

  anim_class->update_paint_view = animation_cel_animation_update_paint_view;

  g_type_class_add_private (klass, sizeof (AnimationCelAnimationPrivate));
}

static void
animation_cel_animation_init (AnimationCelAnimation *animation)
{
  animation->priv = G_TYPE_INSTANCE_GET_PRIVATE (animation,
                                                 ANIMATION_TYPE_CEL_ANIMATION,
                                                 AnimationCelAnimationPrivate);
  animation->priv->camera = animation_camera_new (ANIMATION (animation));
}

static void
animation_cel_animation_constructed (GObject *object)
{
  AnimationCelAnimation *animation = ANIMATION_CEL_ANIMATION (object);

  g_signal_connect (animation->priv->camera, "offsets-changed",
                    G_CALLBACK (on_camera_offsets_changed),
                    animation);
}

static void
animation_cel_animation_finalize (GObject *object)
{
  animation_cel_animation_cleanup (ANIMATION_CEL_ANIMATION (object));

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**** Public Functions ****/

void
animation_cel_animation_set_layers (AnimationCelAnimation *animation,
                                    gint                   level,
                                    gint                   position,
                                    const GList           *new_layers)
{
  Track *track;
  GList *layers;

  track = g_list_nth_data (animation->priv->tracks, level);
  g_return_if_fail (track && position >= 0 &&
                    position < animation->priv->duration);

  layers = g_list_nth (track->frames, position);

  if (! layers)
    {
      gint frames_length = g_list_length (track->frames);
      gint i;

      track->frames = g_list_reverse (track->frames);
      for (i = frames_length; i < position + 1; i++)
        {
          track->frames = g_list_prepend (track->frames, NULL);
          layers = track->frames;
        }
      track->frames = g_list_reverse (track->frames);
    }

  /* Clean out previous layer list. */
  g_list_free (layers->data);
  if (new_layers)
    {
      layers->data = g_list_copy ((GList *) new_layers);
    }
  else
    {
      layers->data = NULL;
    }

  g_signal_emit_by_name (animation, "frames-changed", position, 1);
}

const GList *
animation_cel_animation_get_layers (AnimationCelAnimation *animation,
                                    gint                   level,
                                    gint                   position)
{
  Track *track;

  track = g_list_nth_data (animation->priv->tracks, level);
  g_return_val_if_fail (track && position >= 0 &&
                        position < animation->priv->duration,
                        NULL);

  return g_list_nth_data (track->frames, position);
}

void
animation_cel_animation_set_comment (AnimationCelAnimation *animation,
                                     gint                   position,
                                     const gchar           *comment)
{
  GList *item;

  g_return_if_fail (position >= 0 &&
                    position < animation->priv->duration);

  item = g_list_nth (animation->priv->comments, position);
  if (item && item->data)
    {
      g_free (item->data);
    }
  else if (! item)
    {
      gint length = g_list_length (animation->priv->comments);
      gint i;

      animation->priv->comments = g_list_reverse (animation->priv->comments);
      for (i = length; i < position + 1; i++)
        {
          animation->priv->comments = g_list_prepend (animation->priv->comments, NULL);
          item = animation->priv->comments;
        }
      animation->priv->comments = g_list_reverse (animation->priv->comments);
    }

  item->data = g_strdup (comment);
}

const gchar *
animation_cel_animation_get_comment (AnimationCelAnimation *animation,
                                     gint                   position)
{
  g_return_val_if_fail (position >= 0 &&
                        position < animation->priv->duration,
                        0);

  return g_list_nth_data (animation->priv->comments, position);
}

void
animation_cel_animation_set_onion_skins (AnimationCelAnimation *animation,
                                         gint                   skins)
{
  animation->priv->onion_skins = skins;
}

gint
animation_cel_animation_get_onion_skins (AnimationCelAnimation *animation)
{
  return animation->priv->onion_skins;
}

void
animation_cel_animation_set_duration (AnimationCelAnimation *animation,
                                      gint                   duration)
{
  if (duration < animation->priv->duration)
    {
      GList *iter;

      /* Free memory. */
      iter = g_list_nth (animation->priv->tracks, duration);
      if (iter && iter->prev)
        {
          iter->prev->next = NULL;
          iter->prev = NULL;
        }
      g_list_free_full (iter, (GDestroyNotify) animation_cel_animation_clean_track);

      iter = g_list_nth (animation->priv->comments, duration);
      if (iter && iter->prev)
        {
          iter->prev->next = NULL;
          iter->prev = NULL;
        }
      g_list_free_full (iter, (GDestroyNotify) g_free);

      for (iter = animation->priv->tracks; iter; iter = iter->next)
        {
          Track *track = iter->data;
          GList *iter2;

          iter2 = g_list_nth (track->frames, duration);
          if (iter2 && iter2->prev)
            {
              iter2->prev->next = NULL;
              iter2->prev = NULL;
            }
          g_list_free_full (iter2, (GDestroyNotify) g_list_free);
        }
    }

  if (duration != animation->priv->duration)
    {
      animation->priv->duration = duration;
      g_signal_emit_by_name (animation, "duration-changed",
                             duration);
    }
}

GObject *
animation_cel_animation_get_main_camera (AnimationCelAnimation *animation)
{
  return G_OBJECT (animation->priv->camera);
}

gint
animation_cel_animation_get_levels (AnimationCelAnimation *animation)
{
  return g_list_length (animation->priv->tracks);
}

gint
animation_cel_animation_level_up (AnimationCelAnimation *animation,
                                  gint                   level)
{
  GList *track;
  GList *prev_track;
  GList *next_track;
  GList *iter;
  gint   i;

  g_return_val_if_fail (level >= 0 &&
                        level < g_list_length (animation->priv->tracks) - 1,
                        level);

  track = g_list_nth (animation->priv->tracks, level);
  prev_track = track->prev;
  next_track = track->next;

  if (prev_track)
    prev_track->next = next_track;
  else
    animation->priv->tracks = next_track;
  next_track->prev = prev_track;
  track->prev      = next_track;
  track->next      = next_track->next;
  next_track->next = track;

  level++;

  iter  = ((Track *) track->data)->frames;
  for (i = 0; iter; iter = iter->next, i++)
    {
      if (iter->data)
        {
          /* Only cache if the track had contents for this frame. */
          g_signal_emit_by_name (animation, "frames-changed", i, 1);
        }
    }

  return level;
}

gint
animation_cel_animation_level_down (AnimationCelAnimation *animation,
                                    gint                   level)
{
  GList *track;
  GList *prev_track;
  GList *next_track;
  GList *iter;
  gint   i;

  g_return_val_if_fail (level > 0 &&
                        level < g_list_length (animation->priv->tracks),
                        level);

  track = g_list_nth (animation->priv->tracks, level);
  prev_track = track->prev;
  next_track = track->next;

  if (! prev_track->prev)
    animation->priv->tracks = track;
  if (next_track)
    next_track->prev = prev_track;
  prev_track->next = next_track;
  track->next = prev_track;
  track->prev = prev_track->prev;
  prev_track->prev = track;

  level--;

  iter  = ((Track *) track->data)->frames;
  for (i = 0; iter; iter = iter->next, i++)
    {
      if (iter->data)
        {
          /* Only cache if the track had contents for this frame. */
          g_signal_emit_by_name (animation, "frames-changed", i, 1);
        }
    }

  return level;
}

gboolean
animation_cel_animation_level_delete (AnimationCelAnimation *animation,
                                      gint                   level)
{
  gint tracks_n = g_list_length (animation->priv->tracks);

  g_return_val_if_fail (level >= 0 && level < tracks_n, FALSE);

  /* Do not remove when there is only a single level. */
  if (tracks_n > 1)
    {
      Track *track;
      GList *item;
      GList *iter;
      gint   i;

      item = g_list_nth (animation->priv->tracks, level);
      track = item->data;
      animation->priv->tracks = g_list_delete_link (animation->priv->tracks,
                                                    item);
      iter = track->frames;
      for (i = 0; iter; iter = iter->next, i++)
        {
          if (iter->data)
            {
              /* Only cache if the track had contents for this frame. */
              g_signal_emit_by_name (animation, "frames-changed", i, 1);
            }
        }
      animation_cel_animation_clean_track (track);

      return TRUE;
    }
  return FALSE;
}

gboolean
animation_cel_animation_level_add (AnimationCelAnimation *animation,
                                   gint                   level)
{
  Track *track;
  gint   tracks_n = g_list_length (animation->priv->tracks);

  g_return_val_if_fail (level >= 0 && level <= tracks_n, FALSE);

  track = g_new0 (Track, 1);
  track->title = g_strdup (_("Name me"));
  animation->priv->tracks = g_list_insert (animation->priv->tracks,
                                           track, level);

  return TRUE;
}

const gchar *
animation_cel_animation_get_track_title (AnimationCelAnimation *animation,
                                         gint                   level)
{
  gchar *title = NULL;
  GList *track;

  track = g_list_nth (animation->priv->tracks, level);

  if (track)
    {
      title = ((Track *) track->data)->title;
    }

  return title;
}

void
animation_cel_animation_set_track_title (AnimationCelAnimation *animation,
                                         gint                   level,
                                         const gchar           *title)
{
  GList *track;

  track = g_list_nth (animation->priv->tracks, level);

  if (track)
    {
      g_free (((Track *) track->data)->title);
      ((Track *) track->data)->title = g_strdup (title);
    }
}

gboolean
animation_cel_animation_cel_delete (AnimationCelAnimation *animation,
                                    gint                   level,
                                    gint                   position)
{
  Track *track;

  track = g_list_nth_data (animation->priv->tracks, level);

  if (track)
    {
      GList *cel = g_list_nth (track->frames, position);

      if (cel)
        {
          GList *iter;
          gint   i;

          g_list_free (cel->data);
          iter = cel->next;
          track->frames = g_list_delete_link (track->frames, cel);

          for (i = position; iter; iter = iter->next, i++)
            {
              g_signal_emit_by_name (animation, "frames-changed", i, 1);
            }

          return TRUE;
        }
    }
  return FALSE;
}

gboolean
animation_cel_animation_cel_add (AnimationCelAnimation *animation,
                                 gint                   level,
                                 gint                   position,
                                 gboolean               dup_previous)
{
  Track *track;

  track = g_list_nth_data (animation->priv->tracks, level);

  if (track)
    {
      GList *cel;
      GList *contents = NULL;
      gint   i = position;

      if (dup_previous && position > 0)
        {
          GList *prev_cell;

          i++;
          prev_cell = g_list_nth (track->frames, position - 1);

          if (prev_cell)
            contents = g_list_copy (prev_cell->data);
        }
      track->frames = g_list_insert (track->frames, contents, position);

      if (g_list_length (track->frames) > animation->priv->duration &&
          g_list_last (track->frames)->data)
        animation_cel_animation_set_duration (animation,
                                              g_list_length (track->frames));
      cel = g_list_nth (track->frames, i);
      if (cel)
        {
          for (; cel; cel = cel->next, i++)
            {
              g_signal_emit_by_name (animation, "frames-changed", i, 1);
            }
          return TRUE;
        }
    }
  return FALSE;
}

/**** Virtual methods ****/

static gint
animation_cel_animation_get_duration (Animation *animation)
{
  return ANIMATION_CEL_ANIMATION (animation)->priv->duration;
}

static gchar *
animation_cel_animation_get_frame_hash (Animation *animation,
                                        gint       position)
{
  return animation_cel_animation_get_hash (ANIMATION_CEL_ANIMATION (animation),
                                           position, FALSE);
}

static GeglBuffer *
animation_cel_animation_create_frame (Animation *animation,
                                      GObject   *renderer G_GNUC_UNUSED,
                                      gint       position,
                                      gdouble    proxy_ratio)
{
  AnimationCelAnimation *cel_animation;
  GeglBuffer            *buffer = NULL;
  GList                 *iter;
  gint32                 image_id;
  gint                   preview_width;
  gint                   preview_height;
  gint                   offset_x;
  gint                   offset_y;

  cel_animation = ANIMATION_CEL_ANIMATION (animation);
  image_id = animation_get_image_id (animation);
  animation_get_size (animation,
                      &preview_width, &preview_height);
  preview_height *= proxy_ratio;
  preview_width  *= proxy_ratio;
  animation_camera_get (cel_animation->priv->camera,
                        position, &offset_x, &offset_y);

  for (iter = cel_animation->priv->tracks; iter; iter = iter->next)
    {
      Track *track = iter->data;
      GList *layers;
      GList *iter2;

      layers = g_list_nth_data (track->frames, position);

      for (iter2 = layers; iter2; iter2 = iter2->next)
        {
          GeglBuffer *source = NULL;
          GeglBuffer *intermediate;
          gint        layer_offx;
          gint        layer_offy;
          gint32      layer;
          gint        tattoo;

          tattoo = GPOINTER_TO_INT (iter2->data);

          layer = gimp_image_get_layer_by_tattoo (image_id, tattoo);
          if (layer > 0)
            source = gimp_drawable_get_buffer (layer);
          if (layer <= 0 || ! source)
            {
              g_printerr ("Warning: a layer used for frame %d has been deleted.\n",
                          position);
              continue;
            }
          gimp_drawable_offsets (layer, &layer_offx, &layer_offy);
          intermediate = normal_blend (preview_width, preview_height,
                                       buffer, 1.0, 0, 0,
                                       source, proxy_ratio,
                                       layer_offx + offset_x,
                                       layer_offy + offset_y);
          g_object_unref (source);
          if (buffer)
            {
              g_object_unref (buffer);
            }
          buffer = intermediate;
        }
    }
  return buffer;
}

static void
animation_cel_animation_reset_defaults (Animation *animation)
{
  AnimationCelAnimationPrivate *priv;
  Track                        *track;
  gint32                        image_id;
  gint32                        layer;
  gint                          i;

  priv = ANIMATION_CEL_ANIMATION (animation)->priv;
  animation_cel_animation_cleanup (ANIMATION_CEL_ANIMATION (animation));

  priv->camera = animation_camera_new (animation);
  /* Purely arbitrary value. User will anyway change it to suit one's needs. */
  priv->duration = 240;

  /* There are at least 2 tracks. Second one is freely-named. */
  track = g_new0 (Track, 1);
  track->title = g_strdup (_("Name me"));
  priv->tracks = g_list_prepend (priv->tracks, track);
  /* The first track is called "Background". */
  track = g_new0 (Track, 1);
  track->title = g_strdup (_("Background"));
  priv->tracks = g_list_prepend (priv->tracks, track);

  /* If there is a layer named "Background", set it to all frames
   * on background track. */
  image_id = animation_get_image_id (animation);
  layer    = gimp_image_get_layer_by_name (image_id, _("Background"));
  if (layer > 0)
    {
      gint tattoo;

      tattoo = gimp_item_get_tattoo (layer);
      for (i = 0; i < priv->duration; i++)
        {
          GList *layers = NULL;

          layers = g_list_prepend (layers,
                                   GINT_TO_POINTER (tattoo));
          track->frames = g_list_prepend (track->frames,
                                          layers);
        }
    }
}

static gchar *
animation_cel_animation_serialize (Animation   *animation,
                                   const gchar *playback_xml)
{
  AnimationCelAnimationPrivate *priv;
  gchar                        *xml;
  gchar                        *xml2;
  gchar                        *tmp;
  GList                        *iter;
  gint                          width;
  gint                          height;
  gint                          i;

  priv = ANIMATION_CEL_ANIMATION (animation)->priv;

  animation_get_size (animation, &width, &height);
  xml = g_strdup_printf ("<animation type=\"cels\" framerate=\"%f\" "
                          " duration=\"%d\" onion-skins=\"%d\""
                          " width=\"%d\" height=\"%d\">%s",
                          animation_get_framerate (animation),
                          priv->duration, priv->onion_skins,
                          width, height, playback_xml);

  for (iter = priv->tracks; iter; iter = iter->next)
    {
      Track *track = iter->data;
      GList *iter2;
      gint   position;
      gint   duration;

      xml2 = g_markup_printf_escaped ("<sequence name=\"%s\">",
                                      track->title);
      tmp = xml;
      xml = g_strconcat (xml, xml2, NULL);
      g_free (tmp);
      g_free (xml2);

      position = 1;
      duration = 0;
      for (iter2 = track->frames; iter2; iter2 = iter2->next)
        {
          GList *layers = iter2->data;

          if (layers)
            {
              gboolean next_identical = FALSE;

              duration++;

              if (iter2->next && iter2->next->data &&
                  g_list_length (layers) == g_list_length (iter2->next->data))
                {
                  GList *layer1 = layers;
                  GList *layer2 = iter2->next->data;

                  next_identical = TRUE;
                  for (; layer1; layer1 = layer1->next, layer2 = layer2->next)
                    {
                      if (layer1->data != layer2->data)
                        {
                          next_identical = FALSE;
                          break;
                        }
                    }
                }
              if (! next_identical)
                {
                  /* Open tag. */
                  xml2 = g_markup_printf_escaped ("<frame position=\"%d\""
                                                  " duration=\"%d\">",
                                                  position - duration,
                                                  duration);
                  tmp = xml;
                  xml = g_strconcat (xml, xml2, NULL);
                  g_free (tmp);
                  g_free (xml2);

                  for (; layers; layers = layers->next)
                    {
                      gint tattoo = GPOINTER_TO_INT (layers->data);

                      xml2 = g_markup_printf_escaped ("<layer id=\"%d\"/>",
                                                      tattoo);
                      tmp = xml;
                      xml = g_strconcat (xml, xml2, NULL);
                      g_free (tmp);
                      g_free (xml2);
                    }

                  /* End tag. */
                  xml2 = g_markup_printf_escaped ("</frame>");
                  tmp = xml;
                  xml = g_strconcat (xml, xml2, NULL);
                  g_free (tmp);
                  g_free (xml2);

                  duration = 0;
                }
            }
          position++;
        }

      tmp = xml;
      xml = g_strconcat (xml, "</sequence>", NULL);
      g_free (tmp);
    }

  tmp = xml;
  xml = g_strconcat (xml, "<camera>", NULL);
  g_free (tmp);

  for (i = 0; i < priv->duration; i++)
    {
      if (animation_camera_has_keyframe (priv->camera, i))
        {
          gint offset_x;
          gint offset_y;

          animation_camera_get (priv->camera,
                                i, &offset_x, &offset_y);
          xml2 = g_markup_printf_escaped ("<keyframe " "position=\"%d\""
                                          " x=\"%d\" y=\"%d\"/>",
                                          i, offset_x, offset_y);
          tmp = xml;
          xml = g_strconcat (xml, xml2, NULL);
          g_free (tmp);
          g_free (xml2);
        }
    }

  tmp = xml;
  xml = g_strconcat (xml, "</camera>", NULL);
  g_free (tmp);

  tmp = xml;
  xml = g_strconcat (xml, "<comments title=\"\">", NULL);
  g_free (tmp);

  /* New loop for comments. */
  for (iter = priv->comments, i = 0; iter; iter = iter->next, i++)
    {
      if (iter->data && strlen (iter->data) > 0)
        {
          gchar *comment = iter->data;

          xml2 = g_markup_printf_escaped ("<comment frame-position=\"%d\">%s</comment>",
                                          i, comment);
          tmp = xml;
          xml = g_strconcat (xml, xml2, NULL);
          g_free (tmp);
          g_free (xml2);
        }
    }
  tmp = xml;
  xml = g_strconcat (xml, "</comments></animation>", NULL);
  g_free (tmp);

  return xml;
}

static gboolean
animation_cel_animation_deserialize (Animation    *animation,
                                     const gchar  *xml,
                                     GError      **error)
{
  const GMarkupParser markup_parser =
    {
      animation_cel_animation_start_element,
      animation_cel_animation_end_element,
      animation_cel_animation_text,
      NULL,  /*  passthrough  */
      NULL   /*  error        */
    };
  GMarkupParseContext *context;
  ParseStatus          status = { 0, };

  g_return_val_if_fail (xml != NULL && *error == NULL, FALSE);

  /* Parse XML to update. */
  status.state = START_STATE;
  status.animation = animation;

  context = g_markup_parse_context_new (&markup_parser,
                                        0, &status, NULL);
  g_markup_parse_context_parse (context, xml, strlen (xml), error);
  if (*error == NULL)
    {
      AnimationCelAnimation *cel_animation;

      g_markup_parse_context_end_parse (context, error);

      cel_animation = ANIMATION_CEL_ANIMATION (animation);
      /* Reverse track order. */
      cel_animation->priv->tracks = g_list_reverse (cel_animation->priv->tracks);

      g_signal_emit_by_name (animation, "frames-changed", 0,
                             cel_animation->priv->duration);
    }
  g_markup_parse_context_free (context);

  return (*error == NULL);
}

static void
animation_cel_animation_update_paint_view (Animation *animation,
                                           gint       position)
{
  AnimationCelAnimation *cel_animation;
  gint                  *layers;
  GList                 *iter;
  gchar                 *prev_hash;
  gint                   num_layers;
  gint32                 image_id;
  gint                   last_layer;
  gint                   skin = 0;
  gint                   i;

  cel_animation = ANIMATION_CEL_ANIMATION (animation);
  image_id = animation_get_image_id (animation);

  /* Hide all layers. */
  layers = gimp_image_get_layers (image_id, &num_layers);
  for (i = 0; i < num_layers; i++)
    {
      hide_item (layers[i], TRUE, TRUE);
    }

  /* Show layers from current position. */
  for (iter = cel_animation->priv->tracks; iter; iter = iter->next)
    {
      Track *track = iter->data;
      GList *frame_layers;
      GList *iter2;

      frame_layers = g_list_nth_data (track->frames, position);

      for (iter2 = frame_layers; iter2; iter2 = iter2->next)
        {
          gint tattoo;
          gint layer;

          tattoo = GPOINTER_TO_INT (iter2->data);
          layer = gimp_image_get_layer_by_tattoo (image_id, tattoo);
          show_layer (layer, GIMP_COLOR_TAG_RED, 1.0);
          last_layer = layer;
        }
    }

  prev_hash = animation_cel_animation_get_hash (cel_animation, position, TRUE);
  for (i = position - 1; skin < cel_animation->priv->onion_skins && i >= 0; i--)
    {
      gchar  *hash;
      gint32  color;

      hash = animation_cel_animation_get_hash (cel_animation, i, TRUE);
      if (g_strcmp0 (hash, prev_hash) == 0)
        {
          g_free (hash);
          continue;
        }
      g_free (prev_hash);
      prev_hash = hash;

      switch (skin)
        {
        case 0: color = GIMP_COLOR_TAG_BROWN; break;
        case 1: color = GIMP_COLOR_TAG_ORANGE; break;
        case 2: color = GIMP_COLOR_TAG_YELLOW; break;
        case 3: color = GIMP_COLOR_TAG_VIOLET; break;
        default: color = GIMP_COLOR_TAG_GRAY; break;
        }
      /* Show layers from previous position (onion skinning). */
      for (iter = cel_animation->priv->tracks; iter; iter = iter->next)
        {
          Track *track = iter->data;
          GList *frame_layers;
          GList *iter2;

          frame_layers = g_list_nth_data (track->frames, i);

          for (iter2 = frame_layers; iter2; iter2 = iter2->next)
            {
              gint tattoo;
              gint layer;

              tattoo = GPOINTER_TO_INT (iter2->data);
              layer = gimp_image_get_layer_by_tattoo (image_id, tattoo);
              if (! gimp_item_get_visible (layer))
                show_layer (layer, color, 0.5 - 0.1 * skin);
            }
        }
      skin++;
    }
  g_free (prev_hash);
  gimp_image_set_active_layer (image_id, last_layer);
}

static void
animation_cel_animation_start_element (GMarkupParseContext  *context,
                                       const gchar          *element_name,
                                       const gchar         **attribute_names,
                                       const gchar         **attribute_values,
                                       gpointer              user_data,
                                       GError              **error)
{
  const gchar                  **names  = attribute_names;
  const gchar                  **values = attribute_values;
  ParseStatus                   *status = (ParseStatus *) user_data;
  AnimationCelAnimation         *animation = ANIMATION_CEL_ANIMATION (status->animation);
  AnimationCelAnimationPrivate  *priv   = GET_PRIVATE (status->animation);

  switch (status->state)
    {
    case START_STATE:
      if (g_strcmp0 (element_name, "animation") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Tag <animation> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      while (*names && *values)
        {
          if (strcmp (*names, "type") == 0)
            {
              if (! **values || strcmp (*values, "cels") != 0)
                {
                  g_set_error (error, 0, 0,
                               _("Unknown animation type: \"%s\"."),
                               *values);
                  return;
                }
            }
          else if (strcmp (*names, "width") == 0 && **values)
            {
              gint width;
              gint height;

              animation_get_size (status->animation, &width, &height);
              width = (gint) g_ascii_strtoull (*values, NULL, 10);
              animation_set_size (status->animation, width, height);
            }
          else if (strcmp (*names, "height") == 0 && **values)
            {
              gint width;
              gint height;

              animation_get_size (status->animation, &width, &height);
              height = (gint) g_ascii_strtoull (*values, NULL, 10);
              animation_set_size (status->animation, width, height);
            }
          else if (strcmp (*names, "framerate") == 0 && **values)
            {
              gdouble fps = g_strtod (*values, NULL);
              if (fps >= MAX_FRAMERATE)
                {
                  /* Let's avoid huge frame rates. */
                  fps = MAX_FRAMERATE;
                }
              else if (fps <= 0)
                {
                  /* Null or negative framerates are impossible. */
                  fps = DEFAULT_FRAMERATE;
                }
              animation_set_framerate (status->animation, fps);
            }
          else if (strcmp (*names, "duration") == 0 && **values)
            {
              gint duration = (gint) g_ascii_strtoull (*values, NULL, 10);

              animation_cel_animation_set_duration (animation, duration);
            }
          else if (strcmp (*names, "onion-skins") == 0 && **values)
            {
              gint skins = (gint) g_ascii_strtoull (*values, NULL, 10);

              animation_cel_animation_set_onion_skins (animation, skins);
            }

          names++;
          values++;
        }
      status->state = ANIMATION_STATE;
      break;
    case ANIMATION_STATE:
      if (g_strcmp0 (element_name, "sequence") == 0)
        {
          status->track = g_new0 (Track, 1);
          while (*names && *values)
            {
              if (strcmp (*names, "name") == 0)
                {
                  status->track->title = g_strdup (*values);
                }
              names++;
              values++;
            }
          priv->tracks = g_list_prepend (priv->tracks, status->track);
          status->state = SEQUENCE_STATE;
        }
      else if (g_strcmp0 (element_name, "comments") == 0)
        {
          status->state = COMMENTS_STATE;
        }
      else if (g_strcmp0 (element_name, "playback") == 0)
        {
          status->state = PLAYBACK_STATE;
        }
      else if (g_strcmp0 (element_name, "camera") == 0)
        {
          status->state = CAMERA_STATE;
        }
      else
        {
          g_set_error (error, 0, 0,
                       _("Tags <sequence> or <comments> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      break;
    case PLAYBACK_STATE:
      /* Leave processing to the playback. */
      break;
    case SEQUENCE_STATE:
      if (g_strcmp0 (element_name, "frame") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Tag <frame> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      status->frame_position = -1;
      status->frame_duration = -1;

      while (*names && *values)
        {
          if (strcmp (*names, "position") == 0 && **values)
            {
              gint position = g_ascii_strtoll (*values, NULL, 10);

              if (position >= 0)
                status->frame_position = position;
            }
          else if (strcmp (*names, "duration") == 0 && **values)
            {
              gint duration = g_ascii_strtoll (*values, NULL, 10);

              if (duration > 0)
                status->frame_duration = duration;
            }

          names++;
          values++;
        }
      if (status->frame_position == -1 ||
          status->frame_duration == -1)
        {
          g_set_error (error, 0, 0,
                       _("Tag <frame> expects the properties: "
                         "position, duration."));
        }
      status->state = FRAME_STATE;
      break;
    case FRAME_STATE:
      if (g_strcmp0 (element_name, "layer") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Tag <layer> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      while (*names && *values)
        {
          if (strcmp (*names, "id") == 0 && **values)
            {
              GList *iter;
              gint   tattoo = g_ascii_strtoll (*values, NULL, 10);
              gint   track_length;
              gint   i;

              track_length = g_list_length (status->track->frames);

              if (track_length < status->frame_position + status->frame_duration)
                {
                  /* Make sure the list is long enough. */
                  status->track->frames = g_list_reverse (status->track->frames);
                  for (i = track_length; i < status->frame_position + status->frame_duration; i++)
                    {
                      status->track->frames = g_list_prepend (status->track->frames,
                                                              NULL);
                    }
                  status->track->frames = g_list_reverse (status->track->frames);
                }
              iter = status->track->frames;
              for (i = 0; i < status->frame_position + status->frame_duration; i++)
                {
                  if (i >= status->frame_position)
                    {
                      GList *layers = iter->data;

                      layers = g_list_append (layers,
                                              GINT_TO_POINTER (tattoo));
                      iter->data = layers;
                    }
                  iter = iter->next;
                }
            }

          names++;
          values++;
        }
      status->state = LAYER_STATE;
      break;
    case LAYER_STATE:
      /* <layer> should have no child tag. */
      g_set_error (error, 0, 0,
                   _("Unexpected child of <layer>: \"%s\"."),
                   element_name);
      return;
    case CAMERA_STATE:
      if (g_strcmp0 (element_name, "keyframe") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Tag <keyframe> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      else
        {
          gboolean has_x    = FALSE;
          gboolean has_y    = FALSE;
          gint     position = -1;
          gint     x;
          gint     y;

          while (*names && *values)
            {
              if (strcmp (*names, "position") == 0 && **values)
                {
                  position = (gint) g_ascii_strtoll (*values, NULL, 10);
                }
              else if (strcmp (*names, "x") == 0 && **values)
                {
                  has_x = TRUE;
                  x = (gint) g_ascii_strtoll (*values, NULL, 10);
                }
              else if (strcmp (*names, "y") == 0 && **values)
                {
                  has_y = TRUE;
                  y = (gint) g_ascii_strtoll (*values, NULL, 10);
                }

              names++;
              values++;
            }
          if (position >= 0 && has_x && has_y)
            animation_camera_set_keyframe (priv->camera, position, x, y);
        }
      status->state = KEYFRAME_STATE;
      break;
    case COMMENTS_STATE:
      if (g_strcmp0 (element_name, "comment") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Tag <comment> expected. "
                         "Got \"%s\" instead."),
                       element_name);
          return;
        }
      status->frame_position = -1;
      status->frame_duration = -1;

      while (*names && *values)
        {
          if (strcmp (*names, "frame-position") == 0 && **values)
            {
              gint position = (gint) g_ascii_strtoll (*values, NULL, 10);

              status->frame_position = position;
              break;
            }

          names++;
          values++;
        }
      status->state = COMMENT_STATE;
      break;
    case COMMENT_STATE:
      /* <comment> should have no child tag. */
      g_set_error (error, 0, 0,
                   _("Unexpected child of <comment>: <\"%s\">."),
                   element_name);
      return;
    case KEYFRAME_STATE:
      /* <keyframe> should have no child tag for now. */
      g_set_error (error, 0, 0,
                   _("Unexpected child of <keyframe>: <\"%s\">."),
                   element_name);
      return;
    default:
      g_set_error (error, 0, 0,
                   _("Unknown state!"));
      break;
    }
}

static void
animation_cel_animation_end_element (GMarkupParseContext *context,
                                     const gchar         *element_name,
                                     gpointer             user_data,
                                     GError             **error)
{
  ParseStatus *status = (ParseStatus *) user_data;

  switch (status->state)
    {
    case SEQUENCE_STATE:
    case COMMENTS_STATE:
    case PLAYBACK_STATE:
    case CAMERA_STATE:
      status->state = ANIMATION_STATE;
      break;
    case FRAME_STATE:
      status->state = SEQUENCE_STATE;
      break;
    case LAYER_STATE:
      status->state = FRAME_STATE;
      break;
    case ANIMATION_STATE:
      status->state = END_STATE;
      break;
    case COMMENT_STATE:
      status->state = COMMENTS_STATE;
      break;
    case KEYFRAME_STATE:
      status->state = CAMERA_STATE;
      break;
    default: /* START/END_STATE */
      /* invalid XML. I expect the parser to raise an error anyway.*/
      break;
    }
}

static void
animation_cel_animation_text (GMarkupParseContext  *context,
                              const gchar          *text,
                              gsize                 text_len,
                              gpointer              user_data,
                              GError              **error)
{
  ParseStatus *status = (ParseStatus *) user_data;
  AnimationCelAnimation *cel_animation = ANIMATION_CEL_ANIMATION (status->animation);

  switch (status->state)
    {
    case COMMENT_STATE:
      if (status->frame_position == -1)
        /* invalid comment tag. */
        break;
      /* Setting comment to a frame. */
      animation_cel_animation_set_comment (cel_animation,
                                           status->frame_position,
                                           text);
      status->frame_position = -1;
      break;
    default:
      /* Ignoring text everywhere else. */
      break;
    }
}

/**** Signal handling ****/

static void
on_camera_offsets_changed (AnimationCamera       *camera,
                           gint                   position,
                           gint                   duration,
                           AnimationCelAnimation *animation)
{
  g_signal_emit_by_name (animation, "frames-changed",
                         position, duration);
}

/**** Utils ****/

static void
animation_cel_animation_cleanup (AnimationCelAnimation *animation)
{
  g_list_free_full (animation->priv->comments,
                    (GDestroyNotify) g_free);
  animation->priv->comments = NULL;
  g_list_free_full (animation->priv->tracks,
                    (GDestroyNotify) animation_cel_animation_clean_track);
  animation->priv->tracks   = NULL;

  if (animation->priv->camera)
    g_object_unref (animation->priv->camera);
}

static void
animation_cel_animation_clean_track (Track *track)
{
  g_free (track->title);
  g_list_free_full (track->frames, (GDestroyNotify) g_list_free);
  g_free (track);
}

static gchar *
animation_cel_animation_get_hash (AnimationCelAnimation *animation,
                                  gint                   position,
                                  gboolean               layers_only)
{
  gchar *hash = g_strdup ("");
  GList *iter;
  gint   main_offset_x;
  gint   main_offset_y;

  animation_camera_get (animation->priv->camera,
                        position, &main_offset_x, &main_offset_y);

  /* Create the new buffer layer composition. */
  for (iter = animation->priv->tracks; iter; iter = iter->next)
    {
      Track *track = iter->data;
      GList *layers;
      GList *layer;

      layers = g_list_nth_data (track->frames, position);

      for (layer = layers; layer; layer = layer->next)
        {
          gint tattoo;

          tattoo = GPOINTER_TO_INT (layer->data);
          if (tattoo)
            {
              gchar *tmp = hash;
              if (layers_only)
                {
                  hash = g_strdup_printf ("%s%d;",
                                          hash, tattoo);
                }
              else
                {
                  hash = g_strdup_printf ("%s[%d,%d]%d;",
                                          hash,
                                          main_offset_x, main_offset_y,
                                          tattoo);
                }
              g_free (tmp);
            }
        }
    }
  if (strlen (hash) == 0)
    {
      g_free (hash);
      hash = NULL;
    }
  return hash;
}
