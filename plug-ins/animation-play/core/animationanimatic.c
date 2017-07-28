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
#include "animationanimatic.h"

typedef enum
{
  START_STATE,
  ANIMATION_STATE,
  SEQUENCE_STATE,
  PANEL_STATE,
  LAYER_STATE,
  END_SEQUENCE_STATE,
  COMMENTS_STATE,
  COMMENT_STATE,
  END_STATE
} AnimationParseState;

typedef struct
{
  Animation           *animation;
  AnimationParseState  state;

  gint                 panel;
  gint                 duration;

  gint                 xml_level;
} ParseStatus;

enum
{
  IMAGE_DURATION,
  LAST_SIGNAL
};

typedef struct _AnimationAnimaticPrivate AnimationAnimaticPrivate;

struct _AnimationAnimaticPrivate
{
  gdouble      proxy_ratio;

  /* Panels are cached as GEGL buffers. */
  GeglBuffer **cache;
  /* The number of panels. */
  gint         n_panels;
  /* Layers associated to each panel. For serialization. */
  gint        *tattoos;
  /* The duration of each panel in frames. */
  gint        *durations;
  /* Whether a panel should get blended together with previous panel. */
  gboolean    *combine;
  /* Panel comments. */
  gchar      **comments;
};

#define GET_PRIVATE(animation) \
        G_TYPE_INSTANCE_GET_PRIVATE (animation, \
                                     ANIMATION_TYPE_ANIMATIC, \
                                     AnimationAnimaticPrivate)

static void         animation_animatic_finalize   (GObject           *object);

/* Virtual methods */

static gint         animation_animatic_get_length  (Animation         *animation);
static void         animation_animatic_get_size    (Animation         *animation,
                                                    gint              *width,
                                                    gint              *height);

static void         animation_animatic_load        (Animation         *animation,
                                                    gdouble            proxy_ratio);
static void         animation_animatic_load_xml    (Animation         *animation,
                                                    const gchar       *xml,
                                                    gdouble            proxy_ratio);
static GeglBuffer * animation_animatic_get_frame   (Animation         *animation,
                                                    gint               pos);
static gchar      * animation_animatic_serialize   (Animation         *animation);

static gboolean     animation_animatic_same        (Animation         *animation,
                                                    gint               previous_pos,
                                                    gint               next_pos);

/* XML parsing */

static void      animation_animatic_start_element (GMarkupParseContext *context,
                                                    const gchar         *element_name,
                                                    const gchar        **attribute_names,
                                                    const gchar        **attribute_values,
                                                    gpointer             user_data,
                                                    GError             **error);
static void      animation_animatic_end_element   (GMarkupParseContext *context,
                                                   const gchar         *element_name,
                                                   gpointer             user_data,
                                                   GError             **error);

static void      animation_animatic_text          (GMarkupParseContext  *context,
                                                   const gchar          *text,
                                                   gsize                 text_len,
                                                   gpointer              user_data,
                                                   GError              **error);

/* Utils */

static void         animation_animatic_cache      (AnimationAnimatic *animation,
                                                   gint               panel,
                                                   gboolean           recursion);
static gint         animation_animatic_get_layer  (AnimationAnimatic *animation,
                                                   gint               pos);

/* Tag handling (from layer names) */

static gint         parse_ms_tag                  (Animation         *animation,
                                                   const gchar       *str);
static gboolean     parse_combine_tag             (const gchar       *str);

static gboolean     is_ms_tag                     (const gchar       *str,
                                                   gint              *duration,
                                                   gint              *taglength);

G_DEFINE_TYPE (AnimationAnimatic, animation_animatic, ANIMATION_TYPE_ANIMATION)

#define parent_class animation_animatic_parent_class

static guint signals[LAST_SIGNAL] = { 0 };

static void
animation_animatic_class_init (AnimationAnimaticClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  AnimationClass *anim_class   = ANIMATION_CLASS (klass);

  /**
   * AnimationAnimatic::image-duration:
   * @animatic: the #AnimationAnimatic.
   * @layer_id: the #GimpLayer id.
   * @duration: the new duration for @layer_id (in number of panels).
   *
   * The ::image-duration will be emitted when the duration of a layer
   * changes. It can be %0 meaning that this layer should not be shown
   * in the reel.
   */
  signals[IMAGE_DURATION] =
    g_signal_new ("image-duration",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  2,
                  G_TYPE_INT,
                  G_TYPE_INT);

  object_class->finalize = animation_animatic_finalize;

  anim_class->get_length  = animation_animatic_get_length;
  anim_class->get_size    = animation_animatic_get_size;
  anim_class->load        = animation_animatic_load;
  anim_class->load_xml    = animation_animatic_load_xml;
  anim_class->get_frame   = animation_animatic_get_frame;
  anim_class->serialize   = animation_animatic_serialize;
  anim_class->same        = animation_animatic_same;

  g_type_class_add_private (klass, sizeof (AnimationAnimaticPrivate));
}

static void
animation_animatic_init (AnimationAnimatic *animation)
{
}

static void
animation_animatic_finalize (GObject *object)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (object);
  gint                      i;

  if (priv->tattoos)
    g_free (priv->tattoos);
  if (priv->durations)
    g_free (priv->durations);
  if (priv->combine)
    g_free (priv->combine);
  if (priv->comments)
    {
      for (i = 0; i < priv->n_panels; i++)
        {
          g_free (priv->comments[i]);
        }
      g_free (priv->comments);
    }
  if (priv->cache)
    {
      for (i = 0; i < priv->n_panels; i++)
        {
          if (priv->cache[i])
            g_object_unref (priv->cache[i]);
        }
      g_free (priv->cache);
    }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**** Public Functions ****/

void
animation_animatic_set_duration (AnimationAnimatic *animatic,
                                 gint               panel_num,
                                 gint               duration)
{
  AnimationAnimaticPrivate *priv           = GET_PRIVATE (animatic);
  Animation                *animation      = ANIMATION (animatic);
  gint                      prev_length    = animation_get_length (animation);
  gint                      playback_start = animation_get_playback_start (animation);
  gint                      playback_stop  = animation_get_playback_stop (animation);
  gint                      position       = animation_get_position (animation);
  gint                      layer_id;
  gint                      length;

  g_return_if_fail (duration >= 0  &&
                    panel_num > 0 &&
                    panel_num <= priv->n_panels);

  layer_id = animation_animatic_get_layer (animatic, position);

  priv->durations[panel_num - 1] = duration;
  length = animation_get_length (animation);

  if (playback_start > length)
    {
      playback_start = animation_get_start_position (animation);
    }
  if (playback_stop > length ||
      playback_stop == prev_length)
    {
      playback_stop = length;
    }
  g_signal_emit (animatic, signals[IMAGE_DURATION], 0,
                 panel_num, duration);
  g_signal_emit_by_name (animatic, "playback-range",
                         playback_start, playback_stop,
                         animation_get_start_position (animation),
                         animation_get_length (animation));
  if (position > length)
    {
      animation_jump (animation, length);
    }
  else if (layer_id != animation_animatic_get_layer (animatic, position))
    {
      GeglBuffer *buffer;

      buffer = animation_get_frame (animation, position);
      g_signal_emit_by_name (animation, "render",
                             position, buffer, TRUE);
      if (buffer)
        g_object_unref (buffer);
    }
}

gint
animation_animatic_get_duration (AnimationAnimatic *animatic,
                                 gint               panel_num)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animatic);

  g_return_val_if_fail (panel_num > 0 &&
                        panel_num <= priv->n_panels,
                        0);

  return priv->durations[panel_num - 1];
}

void
animation_animatic_set_comment (AnimationAnimatic *animatic,
                                gint               panel_num,
                                const gchar       *comment)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animatic);

  g_return_if_fail (panel_num > 0 &&
                    panel_num <= priv->n_panels);

  if (priv->comments[panel_num - 1])
    g_free (priv->comments[panel_num - 1]);

  priv->comments[panel_num - 1] = g_strdup (comment);
}

const gchar *
animation_animatic_get_comment (AnimationAnimatic *animatic,
                                gint               panel_num)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animatic);

  g_return_val_if_fail (panel_num > 0 &&
                        panel_num <= priv->n_panels,
                        0);
  return priv->comments[panel_num - 1];
}

void
animation_animatic_set_combine (AnimationAnimatic *animatic,
                                gint               panel_num,
                                gboolean           combine)
{
  AnimationAnimaticPrivate *priv      = GET_PRIVATE (animatic);

  g_return_if_fail (panel_num > 0 &&
                    panel_num <= priv->n_panels);

  if (priv->combine[panel_num - 1] != combine)
    {
      priv->combine[panel_num - 1] = combine;
      animation_animatic_cache (animatic, panel_num, TRUE);
    }
}

const gboolean
animation_animatic_get_combine (AnimationAnimatic *animatic,
                                gint               panel_num)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animatic);

  g_return_val_if_fail (panel_num > 0 &&
                        panel_num <= priv->n_panels,
                        0);
  return priv->combine[panel_num - 1];
}

gint
animation_animatic_get_panel (AnimationAnimatic *animation,
                              gint               pos)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gint                      count = 0;
  gint                      i     = -1;

  if (pos >= 1       &&
      pos <= animation_animatic_get_length (ANIMATION (animation)))
    {
      for (i = 0; i < priv->n_panels; i++)
        {
          count += priv->durations[i];
          if (count >= pos)
            break;
        }
    }

  if (i != -1 && i < priv->n_panels)
    return i + 1;

  return -1;
}

void animation_animatic_jump_panel (AnimationAnimatic *animation,
                                    gint               panel)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  /* Get the first frame position for a given panel. */
  gint                      pos = 1;
  gint                      i;

  g_return_if_fail (panel <= priv->n_panels);

  for (i = 0; i < panel - 1; i++)
    {
      pos += priv->durations[i];
    }

  animation_jump (ANIMATION (animation), pos);
}

/**** Virtual methods ****/

static gint
animation_animatic_get_length (Animation *animation)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gint                      count = 0;
  gint                      i ;

  for (i = 0; i < priv->n_panels; i++)
    {
      count += priv->durations[i];
    }

  return count;
}

static void
animation_animatic_get_size (Animation *animation,
                             gint      *width,
                             gint      *height)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gint32                    image_id;
  gint                      image_width;
  gint                      image_height;

  image_id = animation_get_image_id (animation);

  image_width  = gimp_image_width (image_id);
  image_height = gimp_image_height (image_id);

  /* Full preview size. */
  *width  = image_width;
  *height = image_height;

  /* Apply proxy ratio. */
  *width  *= priv->proxy_ratio;
  *height *= priv->proxy_ratio;
}

static void
animation_animatic_load (Animation *animation,
                         gdouble    proxy_ratio)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gint32                   *layers;
  gint32                    image_id;
  gint                      i;

  g_return_if_fail (proxy_ratio > 0.0 && proxy_ratio <= 1.0);

  /* Cleaning. */
  if (priv->cache)
    {
      g_free (priv->tattoos);
      g_free (priv->durations);
      g_free (priv->combine);

      for (i = 0; i < priv->n_panels; i++)
        {
          g_free (priv->comments[i]);
          if (priv->cache[i])
            g_object_unref (priv->cache[i]);
        }
      g_free (priv->comments);
      g_free (priv->cache);
    }

  image_id = animation_get_image_id (animation);
  layers   = gimp_image_get_layers (image_id, &priv->n_panels);

  priv->tattoos   = g_try_malloc0_n (priv->n_panels, sizeof (gint));
  priv->durations = g_try_malloc0_n (priv->n_panels, sizeof (gint));
  priv->combine   = g_try_malloc0_n (priv->n_panels, sizeof (gboolean));
  priv->comments  = g_try_malloc0_n (priv->n_panels, sizeof (gchar*));
  priv->cache     = g_try_malloc0_n (priv->n_panels, sizeof (GeglBuffer*));
  if (! priv->tattoos || ! priv->durations ||
      ! priv->combine || ! priv->comments  ||
      ! priv->cache)
    {
      gimp_message (_("Memory could not be allocated to the animatic."));
      gimp_quit ();
      return;
    }
  priv->proxy_ratio = proxy_ratio;

  for (i = 0; i < priv->n_panels; i++)
    {
      gchar    *layer_name;
      gint      duration;
      gboolean  combine;

      g_signal_emit_by_name (animation, "loading",
                             (gdouble) i / ((gdouble) priv->n_panels - 0.999));

      layer_name = gimp_item_get_name (layers[priv->n_panels - (i + 1)]);

      duration = parse_ms_tag (animation, layer_name);
      combine  = parse_combine_tag (layer_name);

      /* Frame duration. */
      priv->tattoos[i]   = gimp_item_get_tattoo (layers[priv->n_panels - (i + 1)]);
      priv->durations[i] = duration;
      priv->combine[i]   = combine;
      /* Layer names are used as default comments. */
      priv->comments[i]  = layer_name;

      /* Panel image. */
      animation_animatic_cache (ANIMATION_ANIMATIC (animation), i + 1, FALSE);
    }
  g_free (layers);
}

static void
animation_animatic_load_xml (Animation   *animation,
                             const gchar *xml,
                             gdouble      proxy_ratio)
{
  const GMarkupParser markup_parser =
    {
      animation_animatic_start_element,
      animation_animatic_end_element,
      animation_animatic_text,
      NULL,  /*  passthrough  */
      NULL   /*  error        */
    };
  GMarkupParseContext *context;
  ParseStatus          status = { 0, };
  GError              *error  = NULL;

  g_return_if_fail (xml != NULL);

  /* Init with a default load. */
  animation_animatic_load (animation, proxy_ratio);

  /* Parse XML to update. */
  status.state = START_STATE;
  status.animation = animation;
  status.xml_level = 0;

  context = g_markup_parse_context_new (&markup_parser,
                                        0, &status, NULL);
  g_markup_parse_context_parse (context, xml, strlen (xml), &error);
  if (error)
    {
      g_warning ("Error parsing XML: %s", error->message);
    }
  else
    {
      g_markup_parse_context_end_parse (context, &error);
      if (error)
        g_warning ("Error parsing XML: %s", error->message);
    }
  g_markup_parse_context_free (context);

  /* If XML parsing failed, just reset the animation. */
  if (error)
    animation_animatic_load (animation, proxy_ratio);
}

static GeglBuffer *
animation_animatic_get_frame (Animation *animation,
                              gint       pos)
{
  AnimationAnimaticPrivate *priv;
  gint                      panel;

  priv = GET_PRIVATE (animation);
  panel = animation_animatic_get_panel (ANIMATION_ANIMATIC (animation),
                                        pos);
  return g_object_ref (priv->cache[panel - 1]);
}

static gchar *
animation_animatic_serialize (Animation *animation)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gchar                    *text;
  gchar                    *tmp;
  gint                      i;

  text = g_strdup_printf ("<animation type=\"animatic\" framerate=\"%f\" "
                          " duration=\"%d\" width=\"\" height=\"\">"
                          "<sequence>",
                          animation_get_framerate (animation),
                          priv->n_panels);
  for (i = 0; i < priv->n_panels; i++)
    {
      gchar  *panel;

      if (priv->combine[i])
        panel = g_markup_printf_escaped ("<panel duration=\"%d\" "
                                         "blend-mode=\"normal\">"
                                         "<layer id=\"%d\"/></panel>",
                                         priv->durations[i],
                                         priv->tattoos[i]);
      else
        panel = g_markup_printf_escaped ("<panel duration=\"%d\">"
                                         "<layer id=\"%d\"/></panel>",
                                         priv->durations[i],
                                         priv->tattoos[i]);

      tmp = text;
      text = g_strconcat (text, panel, NULL);
      g_free (tmp);
      g_free (panel);
    }
  tmp = text;
  text = g_strconcat (text, "</sequence><comments>", NULL);
  g_free (tmp);

  /* New loop for comments. */
  for (i = 0; i < priv->n_panels; i++)
    {
      if (priv->comments[i])
        {
          gchar *comment;

          /* Comments are for a given panel, not for a frame position. */
          comment = g_markup_printf_escaped ("<comment panel=\"%d\">%s</comment>",
                                             i + 1,
                                             priv->comments[i]);
          tmp = text;
          text = g_strconcat (text, comment, NULL);
          g_free (tmp);
          g_free (comment);
        }
    }
  tmp = text;
  text = g_strconcat (text, "</comments></animation>", NULL);
  g_free (tmp);

  return text;
}

static gboolean
animation_animatic_same (Animation *animation,
                         gint       previous_pos,
                         gint       next_pos)
{
  AnimationAnimaticPrivate *priv = GET_PRIVATE (animation);
  gint                      count = 0;
  gboolean                  identical = FALSE;
  gint                      i ;

  for (i = 0; i < priv->n_panels; i++)
    {
      count += priv->durations[i];
      if (count >= previous_pos && count >= next_pos)
        {
          identical = TRUE;
          break;
        }
      else if (count >= previous_pos || count >= next_pos)
        {
          identical = FALSE;
          break;
        }
    }

  return identical;
}

static void
animation_animatic_start_element (GMarkupParseContext *context,
                                  const gchar         *element_name,
                                  const gchar        **attribute_names,
                                  const gchar        **attribute_values,
                                  gpointer             user_data,
                                  GError             **error)
{
  const gchar              **names  = attribute_names;
  const gchar              **values = attribute_values;
  ParseStatus               *status = (ParseStatus *) user_data;
  AnimationAnimaticPrivate  *priv   = GET_PRIVATE (status->animation);
  gboolean                   combine;

  status->xml_level++;
  switch (status->state)
    {
    case START_STATE:
      if (g_strcmp0 (element_name, "animation") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown animation tag: \"%s\"."),
                       element_name);
          return;
        }
      while (*names && *values)
        {
          if (strcmp (*names, "type") == 0)
            {
              if (! **values || strcmp (*values, "animatic") != 0)
                {
                  g_set_error (error, 0, 0,
                               _("Unknown animation type: \"%s\"."),
                               *values);
                  return;
                }
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

          names++;
          values++;
        }
      status->state = ANIMATION_STATE;
      break;
    case ANIMATION_STATE:
      if (g_strcmp0 (element_name, "sequence") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown sequence tag: \"%s\"."),
                       element_name);
          return;
        }
      status->state = SEQUENCE_STATE;
      break;
    case SEQUENCE_STATE:
      if (g_strcmp0 (element_name, "panel") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown panel tag: \"%s\"."),
                       element_name);
          return;
        }
      status->panel++;
      combine = FALSE;
      while (*names && *values)
        {
          if (strcmp (*names, "duration") == 0 && **values)
            {
              gint duration = g_ascii_strtoll (*values, NULL, 10);

              if (duration > 0)
                priv->durations[status->panel - 1] = duration;
            }
          else if (strcmp (*names, "blend-mode") == 0 && **values &&
                   strcmp (*values, "normal") == 0)
            {
              /* Only the "normal" blend mode is supported currently. */
              combine = TRUE;
            }

          names++;
          values++;
        }
      if (priv->combine[status->panel - 1] != combine)
        {
          priv->combine[status->panel - 1] = combine;
          animation_animatic_cache (ANIMATION_ANIMATIC (status->animation),
                                    status->panel, FALSE);
        }
      status->state = PANEL_STATE;
      break;
    case PANEL_STATE:
      if (g_strcmp0 (element_name, "layer") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown layer tag: \"%s\"."),
                       element_name);
          return;
        }
      status->state = LAYER_STATE;
      break;
    case LAYER_STATE:
      /* <layer> should have no child tag. */
      g_set_error (error, 0, 0,
                   _("Unknown layer tag: \"%s\"."),
                   element_name);
      return;
    case END_SEQUENCE_STATE:
      if (g_strcmp0 (element_name, "comments") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown comments tag: \"%s\"."),
                       element_name);
          return;
        }
      status->state = COMMENTS_STATE;
      break;
    case COMMENTS_STATE:
      if (g_strcmp0 (element_name, "comment") != 0)
        {
          g_set_error (error, 0, 0,
                       _("Unknown comment tag: \"%s\"."),
                       element_name);
          return;
        }
      status->panel = -1;
      while (*names && *values)
        {
          if (strcmp (*names, "panel") == 0 && **values)
            {
              gint panel = (gint) g_ascii_strtoll (*values, NULL, 10);

              status->panel = panel;
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
                   _("Unknown layer tag: \"%s\"."),
                   element_name);
      return;
    default:
      g_set_error (error, 0, 0,
                   _("Unknown state!"));
      break;
    }
}

static void
animation_animatic_end_element (GMarkupParseContext *context,
                                const gchar         *element_name,
                                gpointer             user_data,
                                GError             **error)
{
  ParseStatus *status = (ParseStatus *) user_data;

  status->xml_level--;

  switch (status->state)
    {
    case SEQUENCE_STATE:
    case COMMENTS_STATE:
      status->state = END_SEQUENCE_STATE;
      break;
    case PANEL_STATE:
      status->state = SEQUENCE_STATE;
      break;
    case LAYER_STATE:
      status->state = PANEL_STATE;
      break;
    case END_SEQUENCE_STATE:
    case ANIMATION_STATE:
      status->state = END_STATE;
      break;
    case COMMENT_STATE:
      status->state = COMMENTS_STATE;
      break;
    default: /* START/END_STATE */
      /* invalid XML. I expect the parser to raise an error anyway.*/
      break;
    }
}

static void
animation_animatic_text (GMarkupParseContext  *context,
                         const gchar          *text,
                         gsize                 text_len,
                         gpointer              user_data,
                         GError              **error)
{
  ParseStatus *status = (ParseStatus *) user_data;
  AnimationAnimatic *animatic = ANIMATION_ANIMATIC (status->animation);

  switch (status->state)
    {
    case COMMENT_STATE:
      if (status->panel == -1)
        /* invalid comment tag. */
        break;
      /* Setting comment to a panel. */
      animation_animatic_set_comment (animatic,
                                      status->panel,
                                      text);
      status->panel = -1;
      break;
    default:
      /* Ignoring text everywhere else. */
      break;
    }
}

/**** Utils ****/

static void
animation_animatic_cache (AnimationAnimatic *animatic,
                          gint               panel,
                          gboolean           recursion)
{
  AnimationAnimaticPrivate *priv      = GET_PRIVATE (animatic);
  Animation                *animation = ANIMATION (animatic);
  GeglBuffer               *buffer;
  GeglNode                 *graph, *source, *scale, *translate, *target;
  GeglNode                 *backdrop, *blend;
  gint                      layer_offx;
  gint                      layer_offy;
  gdouble                   panel_offx;
  gdouble                   panel_offy;
  gint                      position;
  gint                      preview_width;
  gint                      preview_height;
  gint32                    image_id;
  gint32                    layer;

  image_id = animation_get_image_id (animation);
  layer = gimp_image_get_layer_by_tattoo (image_id,
                                          priv->tattoos[panel - 1]);
  if (! layer)
    {
      g_warning ("Caching failed: a layer must have been deleted.");
      return;
    }

  /* Destroy existing cache. */
  if (priv->cache[panel - 1])
    {
      g_object_unref (priv->cache[panel - 1]);
    }

  /* Panel image. */
  buffer = gimp_drawable_get_buffer (layer);
  animation_get_size (animation, &preview_width, &preview_height);
  priv->cache[panel - 1] = gegl_buffer_new (GEGL_RECTANGLE (0, 0,
                                                            preview_width,
                                                            preview_height),
                                            gegl_buffer_get_format (buffer));
  graph  = gegl_node_new ();
  source = gegl_node_new_child (graph,
                                "operation", "gegl:buffer-source",
                                "buffer", buffer,
                                NULL);
  scale  = gegl_node_new_child (graph,
                                "operation", "gegl:scale-ratio",
                                "sampler", GEGL_SAMPLER_NEAREST,
                                "x", priv->proxy_ratio,
                                "y", priv->proxy_ratio,
                                NULL);

  gimp_drawable_offsets (layer,
                         &layer_offx, &layer_offy);
  panel_offx = layer_offx * priv->proxy_ratio;
  panel_offy = layer_offy * priv->proxy_ratio;
  translate =  gegl_node_new_child (graph,
                                    "operation", "gegl:translate",
                                    "x", panel_offx,
                                    "y", panel_offy,
                                    NULL);

  target = gegl_node_new_child (graph,
                                "operation", "gegl:write-buffer",
                                "buffer", priv->cache[panel - 1],
                                NULL);

  if (panel > 1 && priv->combine[panel - 1])
    {
      backdrop = gegl_node_new_child (graph,
                                      "operation", "gegl:buffer-source",
                                      "buffer", priv->cache[panel - 2],
                                      NULL);
      blend =  gegl_node_new_child (graph,
                                    "operation", "gegl:over",
                                    NULL);
      gegl_node_link_many (source, scale, translate, NULL);
      gegl_node_link_many (backdrop, blend, target, NULL);
      gegl_node_connect_to (translate, "output",
                            blend, "aux");
    }
  else
    {
      gegl_node_link_many (source, scale, translate, target, NULL);
    }
  gegl_node_process (target);

  g_object_unref (graph);
  g_object_unref (buffer);

  /* If next panel is in "combine" mode, it must also be re-cached.
   * And so on, recursively. */
  if (recursion              &&
      panel < priv->n_panels &&
      priv->combine[panel])
    {
      animation_animatic_cache (animatic, panel + 1, TRUE);
    }

  /* Finally re-render if we are currently showing this panel. */
  position = animation_get_position (animation);
  if (animation_animatic_get_panel (animatic, position) == panel)
    {
      buffer = animation_get_frame (animation, position);
      g_signal_emit_by_name (animation, "render",
                             position, buffer, TRUE);
      if (buffer)
        {
          g_object_unref (buffer);
        }
    }
}

static gint
animation_animatic_get_layer (AnimationAnimatic *animation,
                              gint               pos)
{
  AnimationAnimaticPrivate *priv  = GET_PRIVATE (animation);
  gint                      count = 0;
  gint                      i     = -1;

  if (priv->n_panels > 0 &&
      pos >= 1           &&
      pos <= animation_animatic_get_length (ANIMATION (animation)))
    {
      for (i = priv->n_panels - 1; i >= 0; i--)
        {
          count += priv->durations[i];
          if (count >= pos)
            break;
        }
    }

  return i;
}

/**** TAG UTILS ****/

static gint
parse_ms_tag (Animation   *animation,
              const gchar *str)
{
  if (str != NULL)
    {
      gint length = strlen (str);
      gint i;

      for (i = 0; i < length; i++)
        {
          gint duration;
          gint dummy;

          if (is_ms_tag (&str[i], &duration, &dummy))
            {
              gdouble fps = animation_get_framerate (animation);

              /* Get frame duration in frame numbers, not millisecond. */
              duration = (gint) ((fps * (gdouble) duration) / 1000.0);

              return duration;
            }
        }
    }

  /* Default to 6 frames per panel.
   * Storyboard-type animations are rarely detailed. */
  return 6;
}

static gboolean
parse_combine_tag (const gchar *str)
{
  gboolean combine = FALSE;

  if (str != NULL)
    {
      gint length = strlen (str);
      gint i;

      for (i = 0; i < length; i++)
        {
          if (strlen (str) != 9)
            continue;

          if (strncmp (str, "(combine)", 9) == 0)
            {
              combine = TRUE;
              break;
            }
          else if (strncmp (str, "(replace)", 9) == 0)
            {
              combine = FALSE;
              break;
            }
        }
    }

  return combine;
}

static gboolean
is_ms_tag (const gchar *str,
           gint        *duration,
           gint        *taglength)
{
  gint sum = 0;
  gint offset;
  gint length;

  length = strlen(str);

  if (str[0] != '(')
    return FALSE;

  offset = 1;

  /* eat any spaces between open-parenthesis and number */
  while ((offset < length) && (str[offset] == ' '))
    offset++;

  if ((offset>=length) || (!g_ascii_isdigit (str[offset])))
    return FALSE;

  do
    {
      sum *= 10;
      sum += str[offset] - '0';
      offset++;
    }
  while ((offset<length) && (g_ascii_isdigit (str[offset])));

  if (length - offset <= 2)
    return FALSE;

  /* eat any spaces between number and 'ms' */
  while ((offset < length) && (str[offset] == ' '))
    offset++;

  if (length - offset <= 2                     ||
      g_ascii_toupper (str[offset])     != 'M' ||
      g_ascii_toupper (str[offset + 1]) != 'S')
    return FALSE;

  offset += 2;

  /* eat any spaces between 'ms' and close-parenthesis */
  while ((offset < length) && (str[offset] == ' '))
    offset++;

  if ((length - offset < 1) || (str[offset] != ')'))
    return FALSE;

  offset++;

  *duration = sum;
  *taglength = offset;

  return TRUE;
}
