/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * animation-dialog.c
 * Copyright (C) 2015-2016 Jehan <jehan@gimp.org>
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

#undef GDK_DISABLE_DEPRECATED
#include <gtk/gtk.h>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "animation-utils.h"

#include "core/animation.h"
#include "core/animation-animatic.h"
#include "core/animation-camera.h"
#include "core/animation-celanimation.h"
#include "core/animation-playback.h"

#include "animation-dialog.h"
#include "animation-dialog-export.h"
#include "animation-keyframe-view.h"
#include "animation-layer-view.h"
#include "animation-storyboard.h"
#include "animation-xsheet.h"

#include "libgimp/stdplugins-intl.h"

#define DITHERTYPE     GDK_RGB_DITHER_NORMAL

/* for shaping */
typedef struct
{
  gdouble x, y;
} CursorOffset;

enum
{
  PROP_0,
  PROP_IMAGE,
  PROP_ANIMATION
};

typedef struct _AnimationDialogPrivate AnimationDialogPrivate;

struct _AnimationDialogPrivate
{
  gint32             image_id;

  Animation         *animation;
  AnimationPlayback *playback;
  gdouble            zoom;
  gboolean           rendered_once;

  /* GUI */
  GtkWidget         *play_bar;
  GtkWidget         *progress_bar;

  GtkWidget         *progress;
  GtkWidget         *startframe_spin;
  GtkWidget         *endframe_spin;

  /* Bar above the preview. */
  GtkWidget         *upper_bar;
  GtkWidget         *zoomcombo;
  GtkWidget         *refresh;
  GtkWidget         *export;

  GtkWidget         *scrolled_drawing_area;
  GtkWidget         *drawing_area;
  guchar            *drawing_area_data;
  guint              drawing_area_width;
  guint              drawing_area_height;

  GtkWidget         *shape_window;
  GtkWidget         *shape_drawing_area;
  guchar            *shape_drawing_area_data;
  guint              shape_drawing_area_width;
  guint              shape_drawing_area_height;

  /* Notebook on the right (layer list, storyboard, settings). */
  GtkWidget         *right_pane;
  GtkWidget         *right_notebook;
  GtkWidget         *keyframe_view;

  /* Notebook: settings. */
  GtkWidget         *settings;
  GtkWidget         *animation_type_combo;
  GtkWidget         *size_entry;
  GtkWidget         *fpscombo;
  GtkWidget         *onion_spin;
  GtkWidget         *duration_spin;
  GtkWidget         *proxycombo;

  /* Notebook: layer list. */
  GtkWidget         *layer_list;

  /* The left panel (bottom is timeline, above is preview). */
  GtkWidget         *left_pane;
  GtkWidget         *xsheet;

  /* Actions */
  GtkUIManager      *ui_manager;

  GtkActionGroup    *play_actions;
  GtkActionGroup    *settings_actions;
  GtkActionGroup    *view_actions;
  GtkActionGroup    *various_actions;
};

#define GET_PRIVATE(dialog) \
        G_TYPE_INSTANCE_GET_PRIVATE (dialog, \
                                     ANIMATION_TYPE_DIALOG, \
                                     AnimationDialogPrivate)

static void        animation_dialog_constructed  (GObject      *object);
static void        animation_dialog_set_property (GObject      *object,
                                                  guint         property_id,
                                                  const GValue *value,
                                                  GParamSpec   *pspec);
static void        animation_dialog_get_property (GObject      *object,
                                                  guint         property_id,
                                                  GValue       *value,
                                                  GParamSpec   *pspec);
static void        animation_dialog_finalize     (GObject      *object);

/* Initialization. */
static GtkUIManager
                 * ui_manager_new            (AnimationDialog  *dialog);
static void        connect_accelerators      (GtkUIManager     *ui_manager,
                                              GtkActionGroup   *group);

static void        animation_dialog_set_animation (AnimationDialog *dialog,
                                                   Animation       *animation,
                                                   const gchar     *xml);

static gboolean    on_dialog_expose                (GtkWidget      *widget,
                                                    GdkEvent       *event,
                                                    Animation      *animation);
/* Finalization. */
static void        update_ui_sensitivity     (AnimationDialog  *dialog);

/* UI callbacks */
static void        export_callback           (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        close_callback            (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        help_callback             (GtkAction        *action,
                                              AnimationDialog  *dialog);

static void        animation_type_changed    (GtkWidget        *combo,
                                              AnimationDialog  *dialog);

static void        animation_size_changed    (GimpSizeEntry    *gse,
                                              AnimationDialog  *dialog);

static void        on_onion_spin_changed     (GtkAdjustment    *adjustment,
                                              AnimationDialog  *dialog);
static void        on_duration_spin_changed  (GtkAdjustment    *adjustment,
                                              AnimationDialog  *dialog);

static void        fpscombo_activated        (GtkEntry         *combo,
                                              AnimationDialog  *dialog);
static void        fpscombo_changed          (GtkWidget        *combo,
                                              AnimationDialog  *dialog);

static void        zoomcombo_activated       (GtkEntry         *combo,
                                              AnimationDialog  *dialog);
static void        zoomcombo_changed         (GtkWidget        *combo,
                                              AnimationDialog  *dialog);
static void        zoom_in_callback          (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        zoom_out_callback         (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        zoom_reset_callback       (GtkAction        *action,
                                              AnimationDialog  *dialog);

static void        proxycombo_activated      (GtkEntry         *combo_entry,
                                              AnimationDialog  *dialog);
static void        proxycombo_changed        (GtkWidget        *combo,
                                              AnimationDialog  *dialog);

static void        speed_up_callback         (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        speed_down_callback       (GtkAction        *action,
                                              AnimationDialog  *dialog);
static gboolean    adjustment_pressed        (GtkWidget        *widget,
                                              GdkEventButton   *event,
                                              AnimationDialog  *dialog);
static void        startframe_changed        (GtkAdjustment    *adjustment,
                                              AnimationDialog  *dialog);
static void        endframe_changed          (GtkAdjustment    *adjustment,
                                              AnimationDialog  *dialog);

static void        play_callback             (GtkToggleAction  *action,
                                              AnimationDialog  *dialog);
static void        step_back_callback        (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        step_callback             (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        rewind_callback           (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        refresh_callback          (GtkAction        *action,
                                              AnimationDialog  *dialog);
static void        detach_callback           (GtkToggleAction  *action,
                                              AnimationDialog  *dialog);

static gboolean    popup_menu                (GtkWidget        *widget,
                                              AnimationDialog  *dialog);

/* Animation Signals */
static void        show_loading_progress     (Animation         *animation,
                                              gdouble            load_rate,
                                              AnimationDialog   *dialog);
static void        playback_range_changed    (AnimationPlayback *playback,
                                              gint               playback_start,
                                              gint               playback_stop,
                                              AnimationDialog   *dialog);
static void        proxy_changed             (Animation         *animation,
                                              gdouble            fps,
                                              AnimationDialog   *dialog);
static void        framerate_changed         (Animation         *animation,
                                              gdouble            fps,
                                              AnimationDialog   *dialog);
static void        low_framerate_cb          (AnimationPlayback *playback,
                                              gdouble            real_framerate,
                                              AnimationDialog   *dialog);

/* Rendering/Playing Functions */
static gboolean    repaint_da                (GtkWidget        *darea,
                                              GdkEventExpose   *event,
                                              AnimationDialog  *dialog);
static gboolean    da_button_press           (GtkWidget        *widget,
                                              GdkEventButton   *event,
                                              AnimationDialog  *dialog);
static gboolean    da_button_released        (GtkWidget        *widget,
                                              GdkEvent         *event,
                                              AnimationDialog  *dialog);
static gboolean    da_button_motion          (GtkWidget        *widget,
                                              GdkEventMotion   *event,
                                              AnimationDialog  *dialog);
static gboolean    da_scrolled               (GtkWidget        *widget,
                                              GdkEventScroll   *event,
                                              AnimationDialog  *dialog);
static void        da_size_callback          (GtkWidget        *widget,
                                              GtkAllocation    *allocation,
                                              AnimationDialog  *dialog);
static gboolean    shape_pressed             (GtkWidget        *widget,
                                              GdkEventButton   *event,
                                              AnimationDialog  *dialog);
static gboolean    shape_released            (GtkWidget        *widget);
static gboolean    shape_motion              (GtkWidget        *widget,
                                              GdkEventMotion   *event);

static void        render_callback           (AnimationPlayback *animation,
                                              gint              frame_number,
                                              GeglBuffer       *buffer,
                                              gboolean          must_draw_null,
                                              AnimationDialog  *dialog);
static void        render_on_realize         (GtkWidget        *drawing_area,
                                              AnimationDialog  *dialog);
static void        render_frame              (AnimationDialog  *dialog,
                                              GeglBuffer       *buffer,
                                              gboolean          must_draw_null);
static void        reshape_from_bitmap       (AnimationDialog  *dialog,
                                              const gchar      *bitmap);

/* Progress bar interactions */
static gboolean    on_progress_event         (GtkWidget        *widget,
                                              GdkEvent         *event,
                                              AnimationDialog  *dialog);

static void        show_playing_progress     (AnimationDialog  *dialog);

/* Utils */
static void        inactive_on_loading       (Animation       *animation,
                                              gdouble          load_rate,
                                              GtkWidget       *widget);
static void        active_on_loaded          (Animation       *animation,
                                              GtkWidget       *widget);
static void        hide_on_animatic          (AnimationDialog  *dialog,
                                              GParamSpec       *param_spec,
                                              GtkWidget        *widget);
static void        update_progress           (AnimationDialog  *dialog);
static void        block_ui                  (AnimationDialog  *dialog);
static gboolean    is_detached               (AnimationDialog  *dialog);
static void        play_pause                (AnimationDialog  *dialog);

static gdouble     get_fps                   (gint              index);
static gdouble     get_zoom                  (AnimationDialog   *dialog,
                                              gint               index);
static void        update_scale              (AnimationDialog  *dialog,
                                              gdouble           scale);


G_DEFINE_TYPE (AnimationDialog, animation_dialog, GTK_TYPE_WINDOW)

#define parent_class animation_dialog_parent_class

static void
animation_dialog_class_init (AnimationDialogClass *klass)
{
  GObjectClass   *object_class     = G_OBJECT_CLASS (klass);

  object_class->constructed        = animation_dialog_constructed;
  object_class->get_property       = animation_dialog_get_property;
  object_class->set_property       = animation_dialog_set_property;
  object_class->finalize           = animation_dialog_finalize;

  g_object_class_install_property (object_class, PROP_IMAGE,
                                   g_param_spec_int ("image", NULL,
                                                     "GIMP image id",
                                                     G_MININT, G_MAXINT, 0,
                                                     GIMP_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_ANIMATION,
                                   g_param_spec_object ("animation",
                                                        NULL, NULL,
                                                        ANIMATION_TYPE_ANIMATION,
                                                        G_PARAM_READWRITE));

  g_type_class_add_private (klass, sizeof (AnimationDialogPrivate));
}

static void
animation_dialog_init (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  priv->playback = animation_playback_new ();
  priv->rendered_once = FALSE;
}

/**** Public Functions ****/

GtkWidget *
animation_dialog_new (gint32 image_id)
{
  GtkWidget      *dialog;
  Animation      *animation;
  GimpParasite   *parasite;
  gchar          *xml               = NULL;
  gboolean        animatic_selected = TRUE;

  parasite = gimp_image_get_parasite (image_id,
                                      PLUG_IN_PROC "/selected");
  if (parasite)
    {
      const gchar *selected;

      selected = gimp_parasite_data (parasite);
      if (g_strcmp0 (selected, "cel-animation") == 0)
        {
          animatic_selected = FALSE;
        }
      gimp_parasite_free (parasite);
      parasite = NULL;
    }

  if (animatic_selected)
    {
      parasite = gimp_image_get_parasite (image_id,
                                          PLUG_IN_PROC "/animatic");
    }
  else
    {
      parasite = gimp_image_get_parasite (image_id,
                                          PLUG_IN_PROC "/cel-animation");
    }
  if (parasite)
    {
      xml = g_strdup (gimp_parasite_data (parasite));
      gimp_parasite_free (parasite);
    }

  animation = animation_new (image_id, animatic_selected, xml);

  dialog = g_object_new (ANIMATION_TYPE_DIALOG,
                         "type",  GTK_WINDOW_TOPLEVEL,
                         "image", image_id,
                         NULL);
  animation_dialog_set_animation (ANIMATION_DIALOG (dialog),
                                  animation, xml);
  g_free (xml);

  return dialog;
}

/**** Private Functions ****/

static void
animation_dialog_constructed (GObject *object)
{
  AnimationDialog        *dialog = ANIMATION_DIALOG (object);
  AnimationDialogPrivate *priv   = GET_PRIVATE (dialog);
  GtkAdjustment          *adjust;
  GtkWidget              *hpaned;
  GtkWidget              *main_vbox;
  GtkWidget              *abox;
  GtkWidget              *vbox;
  GtkWidget              *hbox;
  GtkWidget              *widget;
  gchar                  *image_name;
  gchar                  *text;
  GdkCursor              *cursor;
  GdkScreen              *screen;
  guint                   screen_width, screen_height;
  gint                    preview_width;
  gint                    preview_height;
  gint                    index;
  gchar                  *icon_path;
  GdkPixbuf              *pixbuf;

  G_OBJECT_CLASS (parent_class)->constructed (object);

  gtk_window_set_role (GTK_WINDOW (dialog), PLUG_IN_ROLE);

  /* Set plugin custom icon. */
  icon_path = g_build_filename (gimp_data_directory (),
                                "plug-ins",
                                PLUG_IN_BINARY,
                                "icons",
                                "gimp-motion.png",
                                NULL);
  pixbuf = gdk_pixbuf_new_from_file (icon_path, NULL);
  gtk_window_set_icon (GTK_WINDOW (dialog), pixbuf);
  g_free (icon_path);
  g_object_unref (pixbuf);

  /* Popup menu */
  g_signal_connect (dialog, "popup-menu",
                    G_CALLBACK (popup_menu),
                    dialog);

  /* Window Title */
  image_name = gimp_image_get_name (priv->image_id);
  text = g_strconcat (_("Animation Playback:"), " ", image_name, NULL);
  gtk_window_set_title (GTK_WINDOW (dialog), text);
  g_free (image_name);
  g_free (text);

  priv->ui_manager = ui_manager_new (dialog);

  /* Window paned. */
  hpaned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_container_add (GTK_CONTAINER (dialog), hpaned);
  gtk_widget_show (hpaned);

  priv->left_pane = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_pack1 (GTK_PANED (hpaned), priv->left_pane, TRUE, TRUE);
  gtk_widget_show (priv->left_pane);

  priv->right_pane = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_paned_pack2 (GTK_PANED (hpaned), priv->right_pane,
                   TRUE, TRUE);
  gtk_widget_show (priv->right_pane);

  priv->right_notebook = gtk_notebook_new ();
  gtk_paned_pack1 (GTK_PANED (priv->right_pane), priv->right_notebook,
                   TRUE, FALSE);
  gtk_widget_show (priv->right_notebook);

  priv->keyframe_view = animation_keyframe_view_new ();
  gtk_paned_pack2 (GTK_PANED (priv->right_pane), priv->keyframe_view,
                   TRUE, FALSE);

  /******************\
  |**** Settings ****|
  \******************/
  priv->settings = gtk_table_new (10, 5, FALSE);
  gtk_notebook_prepend_page (GTK_NOTEBOOK (priv->right_notebook),
                             priv->settings, NULL);
  gtk_notebook_set_tab_label_text (GTK_NOTEBOOK (priv->right_notebook),
                                   priv->settings, _("Settings"));
  /* Settings: animation type */
  widget = gtk_label_new (_("Animation type: "));
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 0, 1, GTK_EXPAND, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  priv->animation_type_combo = gtk_combo_box_text_new ();

  text = _("Animatic");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->animation_type_combo),
                                  text);
  text = _("Cel Animation");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->animation_type_combo),
                                  text);
  gtk_combo_box_set_active (GTK_COMBO_BOX (priv->animation_type_combo), 0);

  g_signal_connect (priv->animation_type_combo, "changed",
                    G_CALLBACK (animation_type_changed),
                    dialog);

  gimp_help_set_help_data (priv->animation_type_combo, _("Animation Type"), NULL);

  gtk_table_attach (GTK_TABLE (priv->settings), priv->animation_type_combo,
                    3, 5, 0, 1, GTK_SHRINK, GTK_SHRINK, 1, 1);
  gtk_widget_show (priv->animation_type_combo);

  /* Settings: animation size. */
  widget = gtk_label_new (_("Animation Size: "));
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 1, 2, GTK_EXPAND, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  priv->size_entry = gimp_size_entry_new (2, GIMP_UNIT_PIXEL, "%a", TRUE, FALSE, FALSE, 7,
                                          GIMP_SIZE_ENTRY_UPDATE_SIZE);
  gimp_size_entry_show_unit_menu (GIMP_SIZE_ENTRY (priv->size_entry), FALSE);
  gimp_size_entry_attach_label (GIMP_SIZE_ENTRY (priv->size_entry),
                                _("Width"), 0, 1, 0.0);
  gimp_size_entry_attach_label (GIMP_SIZE_ENTRY (priv->size_entry),
                                _("Height"), 0, 2, 0.0);
  gimp_size_entry_attach_label (GIMP_SIZE_ENTRY (priv->size_entry),
                                _("px"), 1, 3, 0.5);
  gtk_table_attach (GTK_TABLE (priv->settings), priv->size_entry,
                    3, 5, 1, 2, GTK_EXPAND, GTK_SHRINK, 1, 1);
  g_signal_connect (priv->size_entry,
                    "value-changed",
                    G_CALLBACK (animation_size_changed),
                    dialog);
  gtk_widget_show (priv->size_entry);

  /* Settings: proxy. */
  widget = gtk_label_new (_("Proxy: "));
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 2, 3, GTK_EXPAND, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  priv->proxycombo = gtk_combo_box_text_new_with_entry ();
  gimp_help_set_help_data (priv->proxycombo,
                           _("Degrade image quality for lower memory footprint"),
                           NULL);
  gtk_entry_set_width_chars (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                             8);

  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->proxycombo),
                                  "25 %");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->proxycombo),
                                  "50 %");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->proxycombo),
                                  "75 %");
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->proxycombo),
                                  "100 %");
  /* By default, we are at normal resolution. */
  gtk_combo_box_set_active (GTK_COMBO_BOX (priv->proxycombo), 3);

  g_signal_connect (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                    "activate",
                    G_CALLBACK (proxycombo_activated),
                    dialog);
  g_signal_connect (priv->proxycombo, "changed",
                    G_CALLBACK (proxycombo_changed),
                    dialog);

  gimp_help_set_help_data (priv->proxycombo, _("Proxy resolution quality"), NULL);

  gtk_widget_show (priv->proxycombo);
  gtk_table_attach (GTK_TABLE (priv->settings), priv->proxycombo,
                    3, 5, 2, 3, GTK_SHRINK, GTK_SHRINK, 1, 1);

  /* Settings: fps */
  widget = gtk_label_new (_("Framerate: "));
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 3, 4, GTK_EXPAND, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  priv->fpscombo = gtk_combo_box_text_new_with_entry ();
  gtk_entry_set_width_chars (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                             8);

  for (index = 0; index < 5; index++)
    {
      /* list is given in "fps" - frames per second.
       * Not that fps are double since some common framerates are not
       * integer (in particular 23.976 in NTSC). */
      text = g_strdup_printf  (_("%g fps"), get_fps (index));
      gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->fpscombo), text);
      g_free (text);
    }

  g_signal_connect (priv->fpscombo, "changed",
                    G_CALLBACK (fpscombo_changed),
                    dialog);
  g_signal_connect (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                    "activate",
                    G_CALLBACK (fpscombo_activated),
                    dialog);

  gimp_help_set_help_data (priv->fpscombo, _("Frame Rate"), NULL);

  gtk_table_attach (GTK_TABLE (priv->settings), priv->fpscombo,
                    3, 5, 3, 4, GTK_SHRINK, GTK_SHRINK, 1, 1);
  gtk_widget_show (priv->fpscombo);

  /* Settings: onion skinning */
  widget = gtk_label_new (_("Onion skins:"));
  g_signal_connect (dialog, "notify::animation",
                    G_CALLBACK (hide_on_animatic),
                    widget);
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 4, 5, GTK_SHRINK, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  adjust = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 5.0, 1.0, 2.0, 0.0));
  priv->onion_spin = gtk_spin_button_new (adjust, 0.0, 0.0);
  g_signal_connect (dialog, "notify::animation",
                    G_CALLBACK (hide_on_animatic),
                    priv->onion_spin);
  gtk_entry_set_width_chars (GTK_ENTRY (priv->onion_spin), 1);
  gimp_help_set_help_data (priv->onion_spin, _("Number of skins to show on painting area"), NULL);
  gtk_table_attach (GTK_TABLE (priv->settings), priv->onion_spin,
                    3, 4, 4, 5, GTK_SHRINK, GTK_SHRINK, 1, 1);

  g_signal_connect (adjust,
                    "value-changed",
                    G_CALLBACK (on_onion_spin_changed),
                    dialog);

  gtk_widget_show (priv->onion_spin);

  /* Settings: duration */
  widget = gtk_label_new (_("Duration:"));
  g_signal_connect (dialog, "notify::animation",
                    G_CALLBACK (hide_on_animatic),
                    widget);
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    0, 3, 5, 6, GTK_SHRINK, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  adjust = GTK_ADJUSTMENT (gtk_adjustment_new (240.0, 1.0, G_MAXDOUBLE, 1.0, 10.0, 0.0));
  priv->duration_spin = gtk_spin_button_new (adjust, 0.0, 0.0);
  g_signal_connect (dialog, "notify::animation",
                    G_CALLBACK (hide_on_animatic),
                    priv->duration_spin);
  gtk_entry_set_width_chars (GTK_ENTRY (priv->duration_spin), 5);
  gimp_help_set_help_data (priv->duration_spin, _("Duration in frames"), NULL);
  gtk_table_attach (GTK_TABLE (priv->settings), priv->duration_spin,
                    3, 4, 5, 6, GTK_SHRINK, GTK_SHRINK, 1, 1);

  g_signal_connect (adjust,
                    "value-changed",
                    G_CALLBACK (on_duration_spin_changed),
                    dialog);

  gtk_widget_show (priv->duration_spin);

  widget = gtk_label_new (_("frames"));
  g_signal_connect (dialog, "notify::animation",
                    G_CALLBACK (hide_on_animatic),
                    widget);
  gtk_table_attach (GTK_TABLE (priv->settings), widget,
                    4, 5, 5, 6, GTK_SHRINK, GTK_SHRINK, 1, 1);
  gtk_widget_show (widget);

  gtk_widget_show (priv->settings);
  /**** End of Settings ****/

  /* Playback vertical box. */
  main_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_paned_pack1 (GTK_PANED (priv->left_pane), main_vbox, TRUE, TRUE);
  gtk_widget_show (main_vbox);

  /*************/
  /* Upper bar */
  /*************/
  priv->upper_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_vbox), priv->upper_bar, FALSE, FALSE, 0);
  gtk_widget_show (priv->upper_bar);

  /* Zoom. */
  priv->zoomcombo = gtk_combo_box_text_new_with_entry ();
  gtk_entry_set_width_chars (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->zoomcombo))),
                             5);

  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->zoomcombo),
                                  _("Fit to display"));
  for (index = 1; index < 6; index++)
    {
      text = g_strdup_printf  (_("%.1f %%"), get_zoom (dialog, index) * 100.0);
      gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (priv->zoomcombo), text);
      g_free (text);

      if (get_zoom (dialog, index) == 1.0)
        gtk_combo_box_set_active (GTK_COMBO_BOX (priv->zoomcombo), index);
    }

  g_signal_connect (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->zoomcombo))),
                    "activate",
                    G_CALLBACK (zoomcombo_activated),
                    dialog);
  g_signal_connect (priv->zoomcombo, "changed",
                    G_CALLBACK (zoomcombo_changed),
                    dialog);

  gimp_help_set_help_data (priv->zoomcombo, _("Zoom"), NULL);

  gtk_box_pack_start (GTK_BOX (priv->upper_bar), priv->zoomcombo, FALSE, FALSE, 0);
  gtk_widget_show (priv->zoomcombo);

  /* Export. */
  priv->export = GTK_WIDGET (gtk_tool_button_new (NULL, N_("Export the animation")));

  gtk_activatable_set_related_action (GTK_ACTIVATABLE (priv->export),
                                      gtk_action_group_get_action (priv->various_actions, "export"));

  gtk_box_pack_end (GTK_BOX (priv->upper_bar), priv->export, FALSE, FALSE, 0);
  gtk_widget_show (priv->export);

  /* Refresh. */
  priv->refresh = GTK_WIDGET (gtk_tool_button_new (NULL, N_("Reload the image")));

  gtk_activatable_set_related_action (GTK_ACTIVATABLE (priv->refresh),
                                      gtk_action_group_get_action (priv->settings_actions, "refresh"));

  gtk_box_pack_end (GTK_BOX (priv->upper_bar), priv->refresh, FALSE, FALSE, 0);
  gtk_widget_show (priv->refresh);

  /* Detach. */
  widget = GTK_WIDGET (gtk_toggle_tool_button_new ());
  gtk_tool_button_set_icon_name (GTK_TOOL_BUTTON (widget), GIMP_ICON_DETACH);

  gtk_activatable_set_related_action (GTK_ACTIVATABLE (widget),
                                      gtk_action_group_get_action (priv->view_actions, "detach"));

  gtk_box_pack_end (GTK_BOX (priv->upper_bar), widget, FALSE, FALSE, 0);
  gtk_widget_show (widget);

  /***********/
  /* Drawing */
  /***********/

  /* Vbox for the preview window and lower option bar */
  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_pack_start (GTK_BOX (main_vbox), vbox, TRUE, TRUE, 0);
  gtk_widget_show (vbox);

  /* Alignment for the scrolling window, which can be resized by the user. */
  abox = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
  gtk_box_pack_start (GTK_BOX (vbox), abox, TRUE, TRUE, 0);
  gtk_widget_show (abox);

  priv->scrolled_drawing_area = gtk_scrolled_window_new (NULL, NULL);
  gtk_container_add (GTK_CONTAINER (abox), priv->scrolled_drawing_area);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->scrolled_drawing_area),
                                  GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_show (priv->scrolled_drawing_area);

  /* I add the drawing area inside an alignment box to prevent it from being resized. */
  abox = gtk_alignment_new (0.5, 0.5, 0.0, 0.0);
  gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (priv->scrolled_drawing_area),
                                         abox);
  gtk_widget_show (abox);

  /* Build a drawing area, with a default size same as the image */
  priv->drawing_area = gtk_drawing_area_new ();
  gtk_widget_add_events (priv->drawing_area, GDK_BUTTON_PRESS_MASK);
  gtk_container_add (GTK_CONTAINER (abox), priv->drawing_area);
  gtk_widget_show (priv->drawing_area);

  gtk_widget_add_events (priv->drawing_area,
                         GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                         GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK);
  g_signal_connect (priv->drawing_area, "size-allocate",
                    G_CALLBACK (da_size_callback),
                    dialog);
  g_signal_connect (priv->drawing_area, "button-press-event",
                    G_CALLBACK (da_button_press),
                    dialog);
  g_signal_connect (priv->drawing_area, "button-release-event",
                    G_CALLBACK (da_button_released),
                    dialog);
  g_signal_connect (priv->drawing_area, "motion-notify-event",
                    G_CALLBACK (da_button_motion),
                    dialog);
  g_signal_connect (priv->drawing_area, "scroll-event",
                    G_CALLBACK (da_scrolled),
                    dialog);
  g_object_set_data (G_OBJECT (priv->drawing_area),
                     "cursor-offset", g_new0 (CursorOffset, 1));

  /*****************/
  /* Play toolbar. */
  /*****************/

  hbox = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  /* Without changing resize mode on the toolbar, it would change size when
   * text length in the progress bar changes, and subsequently would the
   * preview display.
   * XXX: gtk_container_set_resize_mode() is deprecated in GTK+ 3.12, but I
   * should check whether this issue still happens before removing the call, or
   * find a new solution.
   */
  gtk_container_set_resize_mode (GTK_CONTAINER (hbox), GTK_RESIZE_QUEUE);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  gtk_widget_show (hbox);

  /* Play buttons. */
  priv->play_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_paned_add1 (GTK_PANED (hbox), priv->play_bar);
  gtk_widget_show (priv->play_bar);

  /* Play: play. */
  widget = GTK_WIDGET (gtk_toggle_tool_button_new ());
  gtk_activatable_set_related_action (GTK_ACTIVATABLE (widget),
                                      gtk_action_group_get_action (priv->play_actions, "play"));

  gtk_box_pack_start (GTK_BOX (priv->play_bar), widget, FALSE, FALSE, 0);
  gtk_widget_show (widget);

  /* Play: step backward. */
  widget = GTK_WIDGET (gtk_tool_button_new (NULL, N_("Step back to previous frame")));
  gtk_activatable_set_related_action (GTK_ACTIVATABLE (widget),
                                      gtk_action_group_get_action (priv->play_actions, "step-back"));

  gtk_box_pack_start (GTK_BOX (priv->play_bar), widget, FALSE, FALSE, 0);
  gtk_widget_show (widget);

  /* Play: step forward. */
  widget = GTK_WIDGET (gtk_tool_button_new (NULL, N_("Step to next frame")));
  gtk_activatable_set_related_action (GTK_ACTIVATABLE (widget),
                                      gtk_action_group_get_action (priv->play_actions, "step"));

  gtk_box_pack_start (GTK_BOX (priv->play_bar), widget, FALSE, FALSE, 0);
  gtk_widget_show (widget);

  /* Play: rewind. */
  widget = GTK_WIDGET (gtk_tool_button_new (NULL, N_("Rewind the animation")));
  gtk_activatable_set_related_action (GTK_ACTIVATABLE (widget),
                                      gtk_action_group_get_action (priv->play_actions, "rewind"));

  gtk_box_pack_start (GTK_BOX (priv->play_bar), widget, FALSE, FALSE, 0);
  gtk_widget_show (widget);

  /* Progress box. */
  priv->progress_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_paned_add2 (GTK_PANED (hbox), priv->progress_bar);
  gtk_widget_show (priv->progress_bar);

  /* End frame spin button. */
  adjust = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 1.0, 5.0, 0.0));
  priv->endframe_spin = gtk_spin_button_new (adjust, 1.0, 0);
  gtk_entry_set_width_chars (GTK_ENTRY (priv->endframe_spin), 2);

  gtk_box_pack_end (GTK_BOX (priv->progress_bar), priv->endframe_spin, FALSE, FALSE, 0);
  gtk_spin_button_set_update_policy (GTK_SPIN_BUTTON (priv->endframe_spin), GTK_UPDATE_IF_VALID);
  gtk_widget_show (priv->endframe_spin);

  g_signal_connect (adjust,
                    "value-changed",
                    G_CALLBACK (endframe_changed),
                    dialog);

  g_signal_connect (priv->endframe_spin, "button-press-event",
                    G_CALLBACK (adjustment_pressed),
                    dialog);

  gimp_help_set_help_data (priv->endframe_spin, _("End frame"), NULL);

  /* Progress bar. */
  priv->progress = gtk_progress_bar_new ();
  gtk_widget_add_events (priv->progress, GDK_BUTTON_RELEASE_MASK);
  gtk_box_pack_end (GTK_BOX (priv->progress_bar), priv->progress, TRUE, TRUE, 0);
  gtk_widget_show (priv->progress);

  gtk_widget_add_events (priv->progress,
                         GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
                         GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
  g_signal_connect (priv->progress, "enter-notify-event",
                    G_CALLBACK (on_progress_event),
                    dialog);
  g_signal_connect (priv->progress, "leave-notify-event",
                    G_CALLBACK (on_progress_event),
                    dialog);
  g_signal_connect (priv->progress, "button-press-event",
                    G_CALLBACK (on_progress_event),
                    dialog);
  g_signal_connect (priv->progress, "button-release-event",
                    G_CALLBACK (on_progress_event),
                    dialog);
  g_signal_connect (priv->progress, "motion-notify-event",
                    G_CALLBACK (on_progress_event),
                    dialog);

  /* Start frame spin button. */
  adjust = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 1.0, 5.0, 0.0));
  priv->startframe_spin = gtk_spin_button_new (adjust, 1.0, 0);
  gtk_entry_set_width_chars (GTK_ENTRY (priv->startframe_spin), 2);

  gtk_box_pack_end (GTK_BOX (priv->progress_bar), priv->startframe_spin, FALSE, FALSE, 0);
  gtk_spin_button_set_update_policy (GTK_SPIN_BUTTON (priv->startframe_spin), GTK_UPDATE_IF_VALID);
  gtk_widget_show (priv->startframe_spin);

  g_signal_connect (adjust,
                    "value-changed",
                    G_CALLBACK (startframe_changed),
                    dialog);

  g_signal_connect (GTK_ENTRY (priv->startframe_spin), "button-press-event",
                    G_CALLBACK (adjustment_pressed),
                    dialog);

  gimp_help_set_help_data (priv->startframe_spin, _("Start frame"), NULL);

  /* Finalization. */
  gtk_window_set_resizable (GTK_WINDOW (dialog), TRUE);

  /* Update GUI size. */
  screen = gtk_widget_get_screen (GTK_WIDGET (dialog));
  screen_height = gdk_screen_get_height (screen);
  screen_width = gdk_screen_get_width (screen);

  preview_width = gimp_image_width (priv->image_id);
  preview_height = gimp_image_height (priv->image_id);
  gtk_window_set_default_size (GTK_WINDOW (dialog),
                               MAX (preview_width + 20,
                                    screen_width - 20),
                               MAX (preview_height + 90,
                                    screen_height - 20));

  /* shape_drawing_area for detached feature. */
  priv->shape_window = gtk_window_new (GTK_WINDOW_POPUP);
  gtk_window_set_resizable (GTK_WINDOW (priv->shape_window), FALSE);

  priv->shape_drawing_area = gtk_drawing_area_new ();
  gtk_container_add (GTK_CONTAINER (priv->shape_window), priv->shape_drawing_area);
  gtk_widget_show (priv->shape_drawing_area);
  gtk_widget_add_events (priv->shape_drawing_area, GDK_BUTTON_PRESS_MASK);
  gtk_widget_realize (priv->shape_drawing_area);

  gdk_window_set_back_pixmap (gtk_widget_get_window (priv->shape_window), NULL, FALSE);

  cursor = gdk_cursor_new_for_display (gtk_widget_get_display (priv->shape_window),
                                       GDK_HAND2);
  gdk_window_set_cursor (gtk_widget_get_window (priv->shape_window), cursor);
  gdk_cursor_unref (cursor);

  g_signal_connect (priv->shape_drawing_area, "size-allocate",
                    G_CALLBACK(da_size_callback),
                    dialog);
  g_signal_connect (priv->shape_drawing_area, "scroll-event",
                    G_CALLBACK (da_scrolled),
                    dialog);
  g_signal_connect (priv->shape_window, "button-press-event",
                    G_CALLBACK (shape_pressed),
                    dialog);
  g_signal_connect (priv->shape_window, "button-release-event",
                    G_CALLBACK (shape_released),
                    NULL);
  g_signal_connect (priv->shape_window, "motion-notify-event",
                    G_CALLBACK (shape_motion),
                    NULL);

  g_object_set_data (G_OBJECT (priv->shape_window),
                     "cursor-offset", g_new0 (CursorOffset, 1));

  g_signal_connect (priv->drawing_area, "expose-event",
                    G_CALLBACK (repaint_da),
                    dialog);

  g_signal_connect (priv->shape_drawing_area, "expose-event",
                    G_CALLBACK (repaint_da),
                    dialog);

  /* We request a minimum size *after* having connected the
   * size-allocate signal for correct initialization. */
  gtk_widget_set_size_request (priv->drawing_area, preview_width, preview_height);
  gtk_widget_set_size_request (priv->shape_drawing_area, preview_width, preview_height);
}

static void
animation_dialog_set_property (GObject      *object,
                               guint         property_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  AnimationDialog        *dialog = ANIMATION_DIALOG (object);
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  switch (property_id)
    {
    case PROP_IMAGE:
      priv->image_id = g_value_get_int (value);
      break;
    case PROP_ANIMATION:
      g_clear_object (&priv->animation);
      priv->animation = g_value_get_object (value);
      g_signal_connect (priv->animation, "loading",
                        (GCallback) inactive_on_loading,
                        priv->settings);
      g_signal_connect (priv->animation, "loaded",
                        (GCallback) active_on_loaded,
                        priv->settings);
      g_signal_connect (priv->animation, "loading",
                        (GCallback) inactive_on_loading,
                        priv->upper_bar);
      g_signal_connect (priv->animation, "loaded",
                        (GCallback) active_on_loaded,
                        priv->upper_bar);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
animation_dialog_get_property (GObject      *object,
                               guint         property_id,
                               GValue       *value,
                               GParamSpec   *pspec)
{
  AnimationDialog        *dialog = ANIMATION_DIALOG (object);
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  switch (property_id)
    {
    case PROP_IMAGE:
      g_value_set_int (value, priv->image_id);
      break;
    case PROP_ANIMATION:
      g_value_set_object (value, priv->animation);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
animation_dialog_finalize (GObject *object)
{
  AnimationDialog        *dialog = ANIMATION_DIALOG (object);
  AnimationDialogPrivate *priv   = GET_PRIVATE (dialog);
  gchar                  *playback_xml;

  /* Save first, before cleaning anything. */
  playback_xml = animation_playback_serialize (priv->playback);
  animation_save_to_parasite (priv->animation, playback_xml);
  g_free (playback_xml);

  if (priv->shape_window)
    gtk_widget_destroy (GTK_WIDGET (priv->shape_window));

  if (priv->animation)
    g_object_unref (priv->animation);

  if (priv->playback)
    g_object_unref (priv->playback);

  G_OBJECT_CLASS (parent_class)->finalize (object);

  /* When this window ends, the plugin ends. */
  gtk_main_quit ();
}

static GtkUIManager *
ui_manager_new (AnimationDialog *dialog)
{
  static GtkActionEntry play_entries[] =
  {
    { "step-back", "media-skip-backward",
      N_("Step _back"), "<control>Left", N_("Step back to previous frame"),
      G_CALLBACK (step_back_callback) },

    { "step", "media-skip-forward",
      N_("_Step"), "<control>Right", N_("Step to next frame"),
      G_CALLBACK (step_callback) },

    { "rewind", "media-seek-backward",
      NULL, "<control>Home", N_("Rewind the animation"),
      G_CALLBACK (rewind_callback) },
  };

  static GtkToggleActionEntry play_toggle_entries[] =
  {
    { "play", "media-playback-start",
      NULL, "<control>space", N_("Start playback"),
      G_CALLBACK (play_callback), FALSE },
  };

  static GtkActionEntry settings_entries[] =
  {
    /* Refresh is not really a settings, but it makes sense to be grouped
     * as such, because we want to be able to refresh and set things in the
     * same moments. */
    { "refresh", "view-refresh",
      N_("Refresh"), "<control>R", N_("Reload the image"),
      G_CALLBACK (refresh_callback) },

    {
      "speed-up", NULL,
      N_("Faster"), "<control>bracketright", N_("Increase the speed of the animation"),
      G_CALLBACK (speed_up_callback)
    },
    {
      "speed-down", NULL,
      N_("Slower"), "<control>bracketleft", N_("Decrease the speed of the animation"),
      G_CALLBACK (speed_down_callback)
    },
  };

  static GtkActionEntry view_entries[] =
  {
    { "zoom-in", GIMP_ICON_ZOOM_IN,
      N_("Zoom in"), "<control>plus", N_("Zoom in"),
      G_CALLBACK (zoom_in_callback) },

    { "zoom-in-accel", GIMP_ICON_ZOOM_IN,
      N_("Zoom in"), "<control>KP_Add", N_("Zoom in"),
      G_CALLBACK (zoom_in_callback) },

    { "zoom-out", GIMP_ICON_ZOOM_OUT,
      N_("Zoom out"), "<control>minus", N_("Zoom out"),
      G_CALLBACK (zoom_out_callback) },

    { "zoom-out-accel", GIMP_ICON_ZOOM_OUT,
      N_("Zoom out"), "<control>KP_Subtract", N_("Zoom out"),
      G_CALLBACK (zoom_out_callback) },

    { "zoom-reset", GIMP_ICON_ZOOM_OUT,
      N_("Zoom 1:1"), "<control>equal", N_("Zoom 1:1"),
      G_CALLBACK (zoom_reset_callback) },

    { "zoom-reset-accel", GIMP_ICON_ZOOM_OUT,
      N_("Zoom 1:1"), "<control>KP_Equal", N_("Zoom 1:1"),
      G_CALLBACK (zoom_reset_callback) },
  };

  static GtkToggleActionEntry view_toggle_entries[] =
  {
    { "detach", GIMP_ICON_DETACH,
      N_("Detach"), NULL,
      N_("Detach the animation from the dialog window"),
      G_CALLBACK (detach_callback), FALSE }
  };

  static GtkActionEntry various_entries[] =
  {
    { "export", GIMP_ICON_DOCUMENT_SAVE,
      NULL, "<control>e", N_("Export the video"),
      G_CALLBACK (export_callback) },

    { "help", "help-browser",
      N_("About the animation plug-in"), "question", NULL,
      G_CALLBACK (help_callback) },

    { "close", "window-close",
      N_("Quit"), "<control>W", NULL,
      G_CALLBACK (close_callback)
    },
    {
      "quit", "application-quit",
      N_("Quit"), "<control>Q", NULL,
      G_CALLBACK (close_callback)
    },
  };

  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkUIManager   *ui_manager = gtk_ui_manager_new ();
  GError         *error      = NULL;

  /* All playback related actions. */
  priv->play_actions = gtk_action_group_new ("playback");
  gtk_action_group_set_translation_domain (priv->play_actions, NULL);
  gtk_action_group_add_actions (priv->play_actions,
                                play_entries,
                                G_N_ELEMENTS (play_entries),
                                dialog);
  gtk_action_group_add_toggle_actions (priv->play_actions,
                                       play_toggle_entries,
                                       G_N_ELEMENTS (play_toggle_entries),
                                       dialog);
  connect_accelerators (ui_manager, priv->play_actions);
  gtk_ui_manager_insert_action_group (ui_manager, priv->play_actions, -1);

  /* All settings related actions. */
  priv->settings_actions = gtk_action_group_new ("settings");
  gtk_action_group_set_translation_domain (priv->settings_actions, NULL);
  gtk_action_group_add_actions (priv->settings_actions,
                                settings_entries,
                                G_N_ELEMENTS (settings_entries),
                                dialog);
  connect_accelerators (ui_manager, priv->settings_actions);
  gtk_ui_manager_insert_action_group (ui_manager, priv->settings_actions, -1);

  /* All view actions. */
  priv->view_actions = gtk_action_group_new ("view");
  gtk_action_group_set_translation_domain (priv->view_actions, NULL);
  gtk_action_group_add_actions (priv->view_actions,
                                view_entries,
                                G_N_ELEMENTS (view_entries),
                                dialog);
  gtk_action_group_add_toggle_actions (priv->view_actions,
                                       view_toggle_entries,
                                       G_N_ELEMENTS (view_toggle_entries),
                                       dialog);
  connect_accelerators (ui_manager, priv->view_actions);
  gtk_ui_manager_insert_action_group (ui_manager, priv->view_actions, -1);

  /* Remaining various actions. */
  priv->various_actions = gtk_action_group_new ("various");

  gtk_action_group_set_translation_domain (priv->various_actions, NULL);

  gtk_action_group_add_actions (priv->various_actions,
                                various_entries,
                                G_N_ELEMENTS (various_entries),
                                dialog);
  connect_accelerators (ui_manager, priv->various_actions);
  gtk_ui_manager_insert_action_group (ui_manager, priv->various_actions, -1);

  /* Finalize. */
  gtk_window_add_accel_group (GTK_WINDOW (dialog),
                              gtk_ui_manager_get_accel_group (ui_manager));
  gtk_accel_group_lock (gtk_ui_manager_get_accel_group (ui_manager));

  /* Finally make some limited popup menu. */
  gtk_ui_manager_add_ui_from_string (ui_manager,
                                     "<ui>"
                                     "  <popup name=\"anim-play-popup\" accelerators=\"true\">"
                                     "    <menuitem action=\"refresh\" />"
                                     "    <separator />"
                                     "    <menuitem action=\"zoom-in\" />"
                                     "    <menuitem action=\"zoom-out\" />"
                                     "    <menuitem action=\"zoom-reset\" />"
                                     "    <separator />"
                                     "    <menuitem action=\"speed-up\" />"
                                     "    <menuitem action=\"speed-down\" />"
                                     "    <separator />"
                                     "    <menuitem action=\"help\" />"
                                     "    <separator />"
                                     "    <menuitem action=\"close\" />"
                                     "  </popup>"
                                     "</ui>",
                                     -1, &error);

  if (error)
    {
      g_warning ("error parsing ui: %s", error->message);
      g_clear_error (&error);
    }

  return ui_manager;
}

static void
connect_accelerators (GtkUIManager   *ui_manager,
                      GtkActionGroup *group)
{
  GList *action_list;
  GList *iter;

  action_list = gtk_action_group_list_actions (group);
  iter        = action_list;
  while (iter)
    {
      /* Make sure all the action's accelerator are correctly connected,
       * even when there are no associated UI item. */
      GtkAction *action = GTK_ACTION (iter->data);

      gtk_action_set_accel_group (action,
                                  gtk_ui_manager_get_accel_group (ui_manager));
      gtk_action_connect_accelerator (action);

      iter = iter->next;
    }
  g_list_free (action_list);
}

static void
animation_dialog_set_animation (AnimationDialog *dialog,
                                Animation       *animation,
                                const gchar     *xml)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkWidget              *frame;
  gchar                  *text;
  gdouble                 fps;
  gint                    index;
  gint                    width;
  gint                    height;

  /* Disconnect all handlers on the previous animation. */
  if (priv->animation)
    {
      g_signal_handlers_disconnect_by_func (priv->animation,
                                            G_CALLBACK (proxy_changed),
                                            dialog);
      g_signal_handlers_disconnect_by_func (priv->animation,
                                            G_CALLBACK (framerate_changed),
                                            dialog);
      g_signal_handlers_disconnect_by_func (priv->playback,
                                            G_CALLBACK (playback_range_changed),
                                            dialog);

      g_signal_handlers_disconnect_by_func (priv->animation,
                                            (GCallback) show_loading_progress,
                                            dialog);
      g_signal_handlers_disconnect_by_func (priv->animation,
                                            (GCallback) update_progress,
                                            dialog);
      g_signal_handlers_disconnect_by_func (priv->animation,
                                            G_CALLBACK (render_callback),
                                            dialog);
      g_signal_handlers_disconnect_by_func (priv->playback,
                                            G_CALLBACK (low_framerate_cb),
                                            dialog);
    }

  /* Block all handlers on UI widgets. */
  g_signal_handlers_block_by_func (priv->animation_type_combo,
                                   G_CALLBACK (animation_type_changed),
                                   dialog);

  g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                                   G_CALLBACK (proxycombo_activated),
                                   dialog);
  g_signal_handlers_block_by_func (priv->proxycombo,
                                   G_CALLBACK (proxycombo_changed),
                                   dialog);

  g_signal_handlers_block_by_func (priv->fpscombo,
                                   G_CALLBACK (fpscombo_changed),
                                   dialog);
  g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                   G_CALLBACK (fpscombo_activated),
                                   dialog);

  g_signal_handlers_block_by_func (gtk_bin_get_child (GTK_BIN (priv->zoomcombo)),
                                   G_CALLBACK (zoomcombo_activated),
                                   dialog);
  g_signal_handlers_block_by_func (priv->zoomcombo,
                                   G_CALLBACK (zoomcombo_changed),
                                   dialog);

  g_signal_handlers_block_by_func (priv->progress,
                                   G_CALLBACK (on_progress_event),
                                   dialog);

  g_object_set (dialog,
                "animation", animation,
                NULL);

  /* Settings: display size. */
  g_signal_handlers_block_by_func (priv->size_entry,
                                   G_CALLBACK (animation_size_changed),
                                   dialog);
  animation_get_size (animation, &width, &height);
  gimp_size_entry_set_value (GIMP_SIZE_ENTRY (priv->size_entry),
                             0, (gdouble) width);
  gimp_size_entry_set_value (GIMP_SIZE_ENTRY (priv->size_entry),
                             1, (gdouble) height);

  g_signal_handlers_unblock_by_func (priv->size_entry,
                                     G_CALLBACK (animation_size_changed),
                                     dialog);

  /* Settings: proxy image. */
  g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                                   G_CALLBACK (proxycombo_activated),
                                   dialog);
  g_signal_handlers_unblock_by_func (priv->proxycombo,
                                   G_CALLBACK (proxycombo_changed),
                                   dialog);
  /* Settings: fps */
  fps = animation_get_framerate (priv->animation);

  gtk_combo_box_set_active (GTK_COMBO_BOX (priv->fpscombo), -1);
  for (index = 0; index < 5; index++)
    {
      if (get_fps (index) == fps)
        {
          gtk_combo_box_set_active (GTK_COMBO_BOX (priv->fpscombo),
                                    index);
          break;
        }
    }

  if (gtk_combo_box_get_active (GTK_COMBO_BOX (priv->fpscombo)) == -1)
    {

      text = g_strdup_printf  (_("%g fps"), fps);
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
      g_free (text);
    }

  g_signal_handlers_unblock_by_func (priv->fpscombo,
                                     G_CALLBACK (fpscombo_changed),
                                     dialog);
  g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                     G_CALLBACK (fpscombo_activated),
                                     dialog);

  /* View: zoom. */
  g_signal_handlers_unblock_by_func (gtk_bin_get_child (GTK_BIN (priv->zoomcombo)),
                                     G_CALLBACK (zoomcombo_activated),
                                     dialog);
  g_signal_handlers_unblock_by_func (priv->zoomcombo,
                                     G_CALLBACK (zoomcombo_changed),
                                     dialog);

  /* Progress bar. */
  g_signal_handlers_unblock_by_func (priv->progress,
                                     G_CALLBACK (on_progress_event),
                                     dialog);

  /* The right panel. */
  if (gtk_notebook_get_n_pages (GTK_NOTEBOOK (priv->right_notebook)) > 1)
    gtk_notebook_remove_page (GTK_NOTEBOOK (priv->right_notebook), 0);
  priv->layer_list = NULL;

  if (ANIMATION_IS_ANIMATIC (animation))
    {
      GtkWidget *storyboard;

      /* The Storyboard view. */
      storyboard = animation_storyboard_new (ANIMATION_ANIMATIC (animation),
                                             priv->playback);
      gtk_notebook_prepend_page (GTK_NOTEBOOK (priv->right_notebook),
                                 storyboard, NULL);
      gtk_notebook_set_tab_label_text (GTK_NOTEBOOK (priv->right_notebook),
                                       storyboard, _("Storyboard"));
      gtk_widget_show (storyboard);

      /* The animation type box. */
      gtk_combo_box_set_active (GTK_COMBO_BOX (priv->animation_type_combo), 0);
    }
  else
    {
      GtkWidget *scrolled_win;
      gint       skins;

      scrolled_win = gtk_scrolled_window_new (NULL, NULL);
      gtk_notebook_prepend_page (GTK_NOTEBOOK (priv->right_notebook),
                                 scrolled_win, NULL);
      gtk_widget_show (scrolled_win);

      gtk_notebook_set_tab_label_text (GTK_NOTEBOOK (priv->right_notebook),
                                       scrolled_win, _("Layers"));

      /* The layer list view. */
      priv->layer_list = animation_layer_view_new (priv->image_id);
      gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (scrolled_win),
                                             priv->layer_list);
      animation_layer_view_refresh (ANIMATION_LAYER_VIEW (priv->layer_list));
      gtk_widget_show (priv->layer_list);

      /* The animation type box. */
      gtk_combo_box_set_active (GTK_COMBO_BOX (priv->animation_type_combo), 1);

      /* Settings: onion-skins. */
      skins = animation_cel_animation_get_onion_skins (ANIMATION_CEL_ANIMATION (animation));
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (priv->onion_spin),
                                 (gdouble) skins);
    }

  /* The bottom panel. */
  frame = gtk_paned_get_child2 (GTK_PANED (priv->left_pane));
  if (frame)
    {
      gtk_widget_destroy (frame);
      priv->xsheet = NULL;
    }

  if (ANIMATION_IS_CEL_ANIMATION (animation))
    {
      frame = gtk_frame_new (_("X-Sheet"));
      gtk_paned_pack2 (GTK_PANED (priv->left_pane), frame,
                       TRUE, TRUE);
      gtk_widget_show (frame);

      priv->xsheet = animation_xsheet_new (ANIMATION_CEL_ANIMATION (animation),
                                           priv->playback, priv->layer_list,
                                           priv->keyframe_view);
      gtk_container_add (GTK_CONTAINER (frame), priv->xsheet);
      gtk_widget_show (priv->xsheet);
    }

  /* Animation type. */
  g_signal_handlers_unblock_by_func (priv->animation_type_combo,
                                     G_CALLBACK (animation_type_changed),
                                     dialog);

  /* Animation. */
  g_signal_connect (priv->animation, "framerate-changed",
                    G_CALLBACK (framerate_changed),
                    dialog);
  g_signal_connect (priv->playback, "range",
                    G_CALLBACK (playback_range_changed),
                    dialog);

  g_signal_connect (priv->animation, "loading",
                    (GCallback) show_loading_progress,
                    dialog);
  g_signal_connect_swapped (priv->animation, "loaded",
                            (GCallback) update_progress,
                            dialog);

  /* Playback. */
  g_signal_connect (priv->playback, "proxy-changed",
                    G_CALLBACK (proxy_changed),
                    dialog);
  g_signal_connect (priv->playback, "render",
                    G_CALLBACK (render_callback),
                    dialog);
  g_signal_connect (priv->playback, "low-framerate",
                    G_CALLBACK (low_framerate_cb),
                    dialog);

  /* Set the playback and its default state. */
  animation_playback_set_animation (priv->playback, animation, xml);

  if (gtk_widget_get_realized (GTK_WIDGET (dialog)))
    on_dialog_expose (GTK_WIDGET (dialog), NULL, priv->animation);
  else
    /* Wait for the dialog to be realized because cache loading can
       take time and it is friendlier to have a visible GUI first. */
    g_signal_connect_after (dialog, "expose-event",
                            G_CALLBACK (on_dialog_expose),
                            priv->animation);
}

static gboolean
on_dialog_expose (GtkWidget *dialog,
                  GdkEvent  *event,
                  Animation *animation)
{
  g_signal_handlers_disconnect_by_func (dialog,
                                        G_CALLBACK (on_dialog_expose),
                                        animation);
  animation_load (animation);

  return FALSE;
}

/* Update the tool sensitivity for playing, depending on the number of frames. */
static void
update_ui_sensitivity (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gboolean                animated;

  animated = animation_playback_get_stop (priv->playback) - animation_playback_get_start (priv->playback) > 1;
  /* Play actions only if we selected several frames between start/end. */
  gtk_action_group_set_sensitive (priv->play_actions, animated);
  gtk_widget_set_sensitive (GTK_WIDGET (priv->play_bar), animated);

  /* We can access the progress bar if there are several frames. */
  gtk_widget_set_sensitive (GTK_WIDGET (priv->progress_bar),
                            animation_get_duration (priv->animation) > 1);

  /* Settings are always changeable. */
  gtk_action_group_set_sensitive (priv->settings_actions, TRUE);

  /* View are always meaningfull with at least 1 frame. */
  gtk_action_group_set_sensitive (priv->view_actions,
                                  animation_get_duration (priv->animation) >= 1);
}

/**** UI CALLBACKS ****/

static void
export_callback (GtkAction       *action,
                 AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  animation_dialog_export (GTK_WINDOW (dialog), priv->playback);
}

static void
close_callback (GtkAction       *action,
                AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  animation_playback_stop (priv->playback);
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
help_callback (GtkAction           *action,
               AnimationDialog *dialog)
{
  gimp_standard_help_func (PLUG_IN_PROC, dialog);
}

static void
animation_size_changed (GimpSizeEntry   *gse,
                        AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gint                    width;
  gint                    height;

  width  = (gint) gimp_size_entry_get_value (gse, 0);
  height = (gint) gimp_size_entry_get_value (gse, 1);

  animation_set_size (priv->animation, width, height);
  /* Resize the drawing areas. */
  gtk_widget_set_size_request (priv->drawing_area, width, height);
  gtk_widget_set_size_request (priv->shape_drawing_area, width, height);
  /* Keep identical zoom. */
  update_scale (ANIMATION_DIALOG (dialog),
                get_zoom (ANIMATION_DIALOG (dialog), -1));
}

static void
animation_type_changed (GtkWidget       *combo,
                        AnimationDialog *dialog)
{
  Animation              *animation;
  AnimationDialogPrivate *priv     = GET_PRIVATE (dialog);
  GimpParasite           *parasite = NULL;
  gchar                  *xml      = NULL;
  gint                    index;

  index = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  if (! priv->animation ||
      ! animation_loaded (priv->animation))
    return;

  if (index == 0)
    {
      parasite = gimp_image_get_parasite (priv->image_id,
                                          PLUG_IN_PROC "/animatic");
    }
  else
    {
      parasite = gimp_image_get_parasite (priv->image_id,
                                          PLUG_IN_PROC "/cel-animation");
    }
  if (parasite)
    {
      xml = g_strdup (gimp_parasite_data (parasite));
      gimp_parasite_free (parasite);
    }
  animation = animation_new (priv->image_id, (index == 0), xml);
  animation_dialog_set_animation (ANIMATION_DIALOG (dialog),
                                  animation, xml);
  g_free (xml);
}

static void
on_duration_spin_changed (GtkAdjustment   *adjustment,
                          AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  AnimationCelAnimation  *animation;
  gdouble                 value = gtk_adjustment_get_value (adjustment);

  g_return_if_fail (priv->animation &&
                    ANIMATION_IS_CEL_ANIMATION (priv->animation));

  animation = ANIMATION_CEL_ANIMATION (priv->animation);
  animation_cel_animation_set_duration (animation, (gint) value);
}

static void
on_onion_spin_changed (GtkAdjustment   *adjustment,
                       AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  AnimationCelAnimation  *animation;
  gdouble                 value = gtk_adjustment_get_value (adjustment);

  g_return_if_fail (priv->animation &&
                    ANIMATION_IS_CEL_ANIMATION (priv->animation));

  animation = ANIMATION_CEL_ANIMATION (priv->animation);
  animation_cel_animation_set_onion_skins (animation, (gint) value);
}


/*
 * Callback emitted when the user hits the Enter key of the fps combo.
 */
static void
fpscombo_activated (GtkEntry        *combo_entry,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  const gchar            *active_text;
  gdouble                 fps;

  if (! priv->animation ||
      ! animation_loaded (priv->animation))
    return;

  active_text = gtk_entry_get_text (combo_entry);
  /* Try a text conversion, locale-aware. */
  fps         = g_strtod (active_text, NULL);

  if (fps >= MAX_FRAMERATE)
    {
      /* Let's avoid huge frame rates. */
      fps = MAX_FRAMERATE;
    }
  else if (fps <= 0)
    {
      /* Null or negative framerates are impossible. */
      fps = 0.5;
    }

  animation_set_framerate (priv->animation, fps);
}

static void
fpscombo_changed (GtkWidget       *combo,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gint                    index;

  index = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  if (! priv->animation ||
      ! animation_loaded (priv->animation))
    return;

  /* If no index, user is probably editing by hand. We wait for him to click "Enter". */
  if (index != -1)
    {
      animation_set_framerate (priv->animation, get_fps (index));
    }
}

/*
 * Callback emitted when the user hits the Enter key of the zoom combo.
 */
static void
zoomcombo_activated (GtkEntry        *combo,
                     AnimationDialog *dialog)
{
  update_scale (dialog, get_zoom (dialog, -1));
}

/*
 * Callback emitted when the user selects a zoom in the dropdown,
 * or edits the text entry.
 * We don't want to process manual edits because it greedily emits
 * signals after each character deleted or added.
 */
static void
zoomcombo_changed (GtkWidget       *combo,
                   AnimationDialog *dialog)
{
  gint index = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  /* If no index, user is probably editing by hand. We wait for him to click "Enter". */
  if (index != -1)
    update_scale (dialog, get_zoom (dialog, index));
}

static void
zoom_in_callback (GtkAction       *action,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble  scale = get_zoom (dialog, -1);

  g_signal_handlers_block_by_func (priv->zoomcombo,
                                   G_CALLBACK (zoomcombo_changed),
                                   dialog);
  update_scale (dialog, scale + 0.05);

  g_signal_handlers_unblock_by_func (priv->zoomcombo,
                                     G_CALLBACK (zoomcombo_changed),
                                     dialog);
}

static void
zoom_out_callback (GtkAction       *action,
                   AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble  scale = get_zoom (dialog, -1);

  if (scale > 0.1)
    {
      g_signal_handlers_block_by_func (priv->zoomcombo,
                                       G_CALLBACK (zoomcombo_changed),
                                       dialog);
      update_scale (dialog, scale - 0.05);
      g_signal_handlers_unblock_by_func (priv->zoomcombo,
                                         G_CALLBACK (zoomcombo_changed),
                                         dialog);
    }
}

static void
zoom_reset_callback (GtkAction       *action,
                     AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble  scale = get_zoom (dialog, -1);

  if (scale != 1.0)
    {
      g_signal_handlers_block_by_func (priv->zoomcombo,
                                       G_CALLBACK (zoomcombo_changed),
                                       dialog);
      update_scale (dialog, 1.0);
      g_signal_handlers_unblock_by_func (priv->zoomcombo,
                                         G_CALLBACK (zoomcombo_changed),
                                         dialog);
    }
}

/*
 * Callback emitted when the user hits the Enter key of the fps combo.
 */
static void
proxycombo_activated (GtkEntry        *combo_entry,
                      AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  const gchar            *active_text;
  gchar                  *text;
  gdouble                 ratio;

  active_text = gtk_entry_get_text (combo_entry);
  /* Try a text conversion, locale-aware. */
  ratio       = g_strtod (active_text, NULL);

  ratio = ratio / 100.0;
  if (ratio >= 1.0)
    {
      ratio = 1.0;
    }
  else if (ratio <= 0)
    {
      /* Null or negative ratio are impossible. */
      ratio = 0.1;
    }

  /* Now let's format the text cleanly: "%.1f %%". */
  text = g_strdup_printf  (_("%.1f %%"), ratio * 100.0);
  gtk_entry_set_text (combo_entry, text);
  g_free (text);

  /* Finally set the preview size, unless they were already good. */
  if (animation_playback_get_proxy (priv->playback) != ratio)
    {
      gboolean was_playing;

      was_playing = animation_playback_is_playing (priv->playback);

      animation_playback_set_proxy (priv->playback, ratio);
      update_scale (dialog, get_zoom (dialog, -1));

      if (was_playing)
        {
          /* Initializing frames stopped the playing. I restart it. */
          play_pause (dialog);
        }
    }
}

static void
proxycombo_changed (GtkWidget       *combo,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gint index = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

  /* If no index, user is probably editing by hand. We wait for him to click "Enter". */
  if (index != -1)
    {
      proxycombo_activated (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                            dialog);
    }
}

static void
speed_up_callback (GtkAction       *action,
                   AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble fps = animation_get_framerate (priv->animation);

  if (fps <= MAX_FRAMERATE - 1)
    {
      gchar *text;

      animation_set_framerate (priv->animation, fps + 1.0);
      fps = animation_get_framerate (priv->animation);

      g_signal_handlers_block_by_func (priv->fpscombo,
                                       G_CALLBACK (fpscombo_changed),
                                       dialog);
      g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                       G_CALLBACK (fpscombo_activated),
                                       dialog);

      text = g_strdup_printf  (_("%g fps"), fps);
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
      g_free (text);

      g_signal_handlers_unblock_by_func (priv->fpscombo,
                                         G_CALLBACK (fpscombo_changed),
                                         dialog);
      g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                         G_CALLBACK (fpscombo_activated),
                                         dialog);
    }
}

static void
speed_down_callback (GtkAction       *action,
                     AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble fps = animation_get_framerate (priv->animation);

  if (fps > 1)
    {
      AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
      gchar *text;

      animation_set_framerate (priv->animation, fps - 1.0);
      fps = animation_get_framerate (priv->animation);

      g_signal_handlers_block_by_func (priv->fpscombo,
                                       G_CALLBACK (fpscombo_changed),
                                       dialog);
      g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                       G_CALLBACK (fpscombo_activated),
                                       dialog);

      text = g_strdup_printf  (_("%g fps"), fps);
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
      g_free (text);

      g_signal_handlers_unblock_by_func (priv->fpscombo,
                                         G_CALLBACK (fpscombo_changed),
                                         dialog);
      g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                         G_CALLBACK (fpscombo_activated),
                                         dialog);
    }
}

static gboolean
adjustment_pressed (GtkWidget       *widget,
                    GdkEventButton  *event,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gboolean event_processed = FALSE;

  if (event->type == GDK_BUTTON_PRESS &&
      event->button == 2)
    {
      GtkSpinButton *spin = GTK_SPIN_BUTTON (widget);
      GtkAdjustment *adj = gtk_spin_button_get_adjustment (spin);

      gtk_adjustment_set_value (adj,
                                (gdouble) animation_playback_get_position (priv->playback) + 1.0);

      /* We don't want the middle click to have another usage (in
       * particular, there is likely no need to copy-paste in these spin
       * buttons). */
      event_processed = TRUE;
    }

  return event_processed;
}

static void
startframe_changed (GtkAdjustment   *adjustment,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble                 value = gtk_adjustment_get_value (adjustment);

  if (! priv->animation)
    return;

  animation_playback_set_start (priv->playback, (gint) value - 1);

  update_ui_sensitivity (dialog);
}

static void
endframe_changed (GtkAdjustment   *adjustment,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gdouble                 value = gtk_adjustment_get_value (adjustment);

  if (! priv->animation)
    return;

  animation_playback_set_stop (priv->playback, (gint) value - 1);

  update_ui_sensitivity (dialog);
}

static void
play_callback (GtkToggleAction *action,
               AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (! animation_playback_is_playing (priv->playback))
    {
      gtk_action_set_icon_name (GTK_ACTION (action), "media-playback-pause");
      animation_playback_play (priv->playback);
    }
  else
    {
      gtk_action_set_icon_name (GTK_ACTION (action), "media-playback-start");
      animation_playback_stop (priv->playback);

      /* The framerate combo might have been modified to display slowness
       * warnings. */
      gtk_widget_modify_text (gtk_bin_get_child (GTK_BIN (priv->fpscombo)), GTK_STATE_NORMAL, NULL);
      gimp_help_set_help_data (priv->fpscombo, _("Frame Rate"), NULL);
      framerate_changed (priv->animation, animation_get_framerate (priv->animation), dialog);
    }

  g_object_set (action,
                "tooltip",
                animation_playback_is_playing (priv->playback) ?  _("Pause playback") : _("Start playback"),
                NULL);
}

static void
step_back_callback (GtkAction           *action,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (animation_playback_is_playing (priv->playback))
    play_pause (dialog);
  animation_playback_prev (priv->playback);
}

static void
step_callback (GtkAction       *action,
               AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (animation_playback_is_playing (priv->playback))
    animation_playback_stop (priv->playback);
  animation_playback_next (priv->playback);
}

static void
rewind_callback (GtkAction       *action,
                 AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gboolean                was_playing;

  was_playing = animation_playback_is_playing (priv->playback);

  if (was_playing)
    play_pause (dialog);

  animation_playback_jump (priv->playback, animation_playback_get_start (priv->playback));

  /* If we were playing, start playing again. */
  if (was_playing)
    play_pause (dialog);
}

static void
refresh_callback (GtkAction       *action,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (priv->layer_list)
    {
      animation_layer_view_refresh (ANIMATION_LAYER_VIEW (priv->layer_list));
    }
  animation_load (priv->animation);
}

static void
detach_callback (GtkToggleAction *action,
                 AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gboolean detached = gtk_toggle_action_get_active (action);
  GeglBuffer *buffer;

  if (detached)
    {
      gint x, y;

      gtk_window_set_screen (GTK_WINDOW (priv->shape_window),
                             gtk_widget_get_screen (priv->drawing_area));

      gtk_widget_show (priv->shape_window);

      if (! gtk_widget_get_realized (priv->drawing_area))
        {
          gtk_widget_realize (priv->drawing_area);
        }
      if (! gtk_widget_get_realized (priv->shape_drawing_area))
        {
          gtk_widget_realize (priv->shape_drawing_area);
        }

      gdk_window_get_origin (gtk_widget_get_window (priv->drawing_area), &x, &y);

      gtk_window_move (GTK_WINDOW (priv->shape_window), x + 6, y + 6);

      gdk_window_set_back_pixmap (gtk_widget_get_window (priv->shape_drawing_area), NULL, TRUE);

      /* Set "alpha grid" background. */
      total_alpha_preview (priv->drawing_area_data,
                           priv->drawing_area_width,
                           priv->drawing_area_height);
      repaint_da (priv->drawing_area, NULL, dialog);
    }
  else
    {
      gtk_widget_hide (priv->shape_window);
    }

  /* Force a refresh after detachment/attachment. */
  buffer = animation_playback_get_buffer (priv->playback,
                                          animation_playback_get_position (priv->playback));
  render_frame (dialog, buffer, TRUE);
  /* clean up */
  if (buffer)
    g_object_unref (buffer);
}

static gboolean
popup_menu (GtkWidget       *widget,
            AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkWidget              *menu;

  menu = gtk_ui_manager_get_widget (priv->ui_manager, "/anim-play-popup");

  gtk_menu_set_screen (GTK_MENU (menu), gtk_widget_get_screen (widget));
  gtk_menu_popup (GTK_MENU (menu),
                  NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());

  return TRUE;
}

static void
show_loading_progress (Animation       *animation,
                       gdouble          load_rate,
                       AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gchar *text;

  block_ui (dialog);

  /* update the dialog's progress bar */
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress), load_rate);

  text = g_strdup_printf (_("Loading animation %d %%"), (gint) (load_rate * 100));
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
  g_free (text);

  /* Forcing the UI to update even with intensive computation. */
  while (gtk_events_pending ())
    gtk_main_iteration ();
}

static void
playback_range_changed (AnimationPlayback *playback,
                        gint               playback_start,
                        gint               playback_stop,
                        AnimationDialog   *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkAdjustment *startframe_adjust;
  GtkAdjustment *stopframe_adjust;
  GtkAdjustment *duration_adjust;

  update_progress (dialog);

  startframe_adjust = gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (priv->startframe_spin));
  stopframe_adjust  = gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (priv->endframe_spin));

  g_signal_handlers_block_by_func (startframe_adjust,
                                   G_CALLBACK (endframe_changed),
                                   dialog);
  g_signal_handlers_block_by_func (stopframe_adjust,
                                   G_CALLBACK (endframe_changed),
                                   dialog);
  gtk_adjustment_set_value (startframe_adjust, playback_start + 1.0);
  gtk_adjustment_set_value (stopframe_adjust, playback_stop + 1.0);
  g_signal_handlers_unblock_by_func (startframe_adjust,
                                     G_CALLBACK (endframe_changed),
                                     dialog);
  g_signal_handlers_unblock_by_func (stopframe_adjust,
                                     G_CALLBACK (endframe_changed),
                                     dialog);

  /* The duration adjust for cel animation. */
  duration_adjust  = gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (priv->duration_spin));
  g_signal_handlers_block_by_func (duration_adjust,
                                   G_CALLBACK (on_duration_spin_changed),
                                   dialog);
  gtk_adjustment_set_value (duration_adjust, animation_get_duration (priv->animation));
  g_signal_handlers_unblock_by_func (duration_adjust,
                                     G_CALLBACK (on_duration_spin_changed),
                                     dialog);
  show_playing_progress (dialog);
}

static void
proxy_changed (Animation       *animation,
               gdouble          proxy,
               AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gchar                  *text;

  g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                                   G_CALLBACK (proxycombo_activated),
                                   dialog);
  g_signal_handlers_block_by_func (priv->proxycombo,
                                   G_CALLBACK (proxycombo_changed),
                                   dialog);

  text = g_strdup_printf  (_("%g %%"), proxy * 100.0);
  gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))), text);
  g_free (text);

  g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->proxycombo))),
                                   G_CALLBACK (proxycombo_activated),
                                   dialog);
  g_signal_handlers_unblock_by_func (priv->proxycombo,
                                   G_CALLBACK (proxycombo_changed),
                                   dialog);
}

static void
framerate_changed (Animation        *animation,
                   gdouble           fps,
                   AnimationDialog  *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gchar                  *text;

  g_signal_handlers_block_by_func (priv->fpscombo,
                                   G_CALLBACK (fpscombo_changed),
                                   dialog);
  g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                   G_CALLBACK (fpscombo_activated),
                                   dialog);
  text = g_strdup_printf  (_("%g fps"), fps);
  gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
  g_free (text);
  g_signal_handlers_unblock_by_func (priv->fpscombo,
                                     G_CALLBACK (fpscombo_changed),
                                     dialog);
  g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                     G_CALLBACK (fpscombo_activated),
                                     dialog);
}

static void
low_framerate_cb (AnimationPlayback *playback,
                  gdouble            real_framerate,
                  AnimationDialog   *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gchar                  *text;

  g_signal_handlers_block_by_func (priv->fpscombo,
                                   G_CALLBACK (fpscombo_changed),
                                   dialog);
  g_signal_handlers_block_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                   G_CALLBACK (fpscombo_activated),
                                   dialog);
  if (real_framerate >= animation_get_framerate (priv->animation))
    {
      /* Reset to normal color. */
      gtk_widget_modify_text (gtk_bin_get_child (GTK_BIN (priv->fpscombo)), GTK_STATE_NORMAL, NULL);
      gimp_help_set_help_data (priv->fpscombo, _("Frame Rate"), NULL);

      text = g_strdup_printf  (_("%g fps"), real_framerate);
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
    }
  else
    {
      GdkColor gdk_red;

      gdk_red.red = 65535;
      gdk_red.green = 0;
      gdk_red.blue = 0;

      gtk_widget_modify_text (gtk_bin_get_child (GTK_BIN (priv->fpscombo)), GTK_STATE_NORMAL, &gdk_red);
      text = g_strdup_printf  (_("%g fps"), real_framerate);
      gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))), text);
      gtk_widget_set_tooltip_text (priv->fpscombo,
                                   _ ("Playback is too slow. We would drop a frame if frame dropping were implemented."));
    }
  g_signal_handlers_unblock_by_func (priv->fpscombo,
                                     G_CALLBACK (fpscombo_changed),
                                     dialog);
  g_signal_handlers_unblock_by_func (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->fpscombo))),
                                     G_CALLBACK (fpscombo_activated),
                                     dialog);
}

/* Rendering Functions */

static gboolean
repaint_da (GtkWidget       *darea,
            GdkEventExpose  *event,
            AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkStyle               *style = gtk_widget_get_style (darea);
  guchar                 *da_data;
  gint                    da_width;
  gint                    da_height;
  gint                    preview_width, preview_height;

  animation_playback_get_size (priv->playback,
                               &preview_width, &preview_height);
  if (darea == priv->drawing_area)
    {
      da_width  = priv->drawing_area_width;
      da_height = priv->drawing_area_height;
      da_data   = priv->drawing_area_data;
    }
  else
    {
      da_width  = priv->shape_drawing_area_width;
      da_height = priv->shape_drawing_area_height;
      da_data   = priv->shape_drawing_area_data;
    }

  gdk_draw_rgb_image (gtk_widget_get_window (darea),
                      style->white_gc,
                      (gint) ((da_width - priv->zoom * preview_width) / 2),
                      (gint) ((da_height - priv->zoom * preview_height) / 2),
                      da_width, da_height,
                      (animation_get_duration (priv->animation) == 1) ? GDK_RGB_DITHER_MAX : DITHERTYPE,
                      da_data, da_width * 3);

  return TRUE;
}

static gboolean
da_button_press (GtkWidget       *widget,
                 GdkEventButton  *event,
                 AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (gdk_event_triggers_context_menu ((GdkEvent *) event))
    {
      GtkWidget *menu = gtk_ui_manager_get_widget (priv->ui_manager, "/anim-play-popup");

      gtk_menu_set_screen (GTK_MENU (menu), gtk_widget_get_screen (widget));
      gtk_menu_popup (GTK_MENU (menu),
                      NULL, NULL, NULL, NULL,
                      event->button,
                      event->time);
      return TRUE;
    }
  else if (event->type == GDK_BUTTON_PRESS &&
           ANIMATION_IS_CEL_ANIMATION (priv->animation))
    {
      CursorOffset *p = g_object_get_data (G_OBJECT (widget),
                                           "cursor-offset");

      if (! p)
        return FALSE;

      p->x = event->x;
      p->y = event->y;

      gtk_grab_add (widget);
      gdk_pointer_grab (gtk_widget_get_window (widget), TRUE,
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_BUTTON_MOTION_MASK  |
                        GDK_POINTER_MOTION_HINT_MASK,
                        NULL, NULL, 0);
      return TRUE;
    }

  return FALSE;
}

static gboolean
da_button_released (GtkWidget       *widget,
                    GdkEvent        *event,
                    AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (ANIMATION_IS_CEL_ANIMATION (priv->animation))
    {
      AnimationCelAnimation  *animation;
      AnimationCamera        *camera;

      animation = ANIMATION_CEL_ANIMATION (priv->animation);
      camera = ANIMATION_CAMERA (animation_cel_animation_get_main_camera (animation));
      animation_camera_apply_preview (camera);

      gtk_grab_remove (widget);
      gdk_display_pointer_ungrab (gtk_widget_get_display (widget), 0);
      gdk_flush ();
    }

  return FALSE;
}

static gboolean
da_button_motion (GtkWidget       *widget,
                  GdkEventMotion  *event,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (! ANIMATION_IS_CEL_ANIMATION (priv->animation))
    return FALSE;

  /* if a button is still held by the time we process this event... */
  if (event->state & GDK_BUTTON1_MASK)
    {
      CursorOffset *p = g_object_get_data (G_OBJECT (widget),
                                           "cursor-offset");

      if (! p)
        return FALSE;

      if (ANIMATION_IS_CEL_ANIMATION (priv->animation))
        {
          AnimationCelAnimation *animation;
          AnimationCamera       *camera;
          gint                   position;
          gint                   x_offset;
          gint                   y_offset;

          animation = ANIMATION_CEL_ANIMATION (priv->animation);
          camera = ANIMATION_CAMERA (animation_cel_animation_get_main_camera (animation));
          position = animation_playback_get_position (priv->playback);
          animation_camera_get (camera, position, &x_offset, &y_offset);
          animation_camera_preview_keyframe (camera, position,
                                             x_offset + (event->x - p->x) / priv->zoom,
                                             y_offset + (event->y - p->y) / priv->zoom);
          p->x = event->x;
          p->y = event->y;
          return TRUE;
        }
    }
  else /* the user has released all buttons */
    {
      da_button_released (widget, NULL, dialog);
    }

  return FALSE;
}

static gboolean
da_scrolled (GtkWidget       *widget,
             GdkEventScroll  *event,
             AnimationDialog *dialog)
{
  if (event->state & GDK_CONTROL_MASK)
    {
      switch (event->direction)
        {
        case GDK_SCROLL_DOWN:
          zoom_out_callback (NULL, dialog);
          break;
        case GDK_SCROLL_UP:
          zoom_in_callback (NULL, dialog);
          break;
        default:
          break;
        }
      return TRUE;
    }
  return FALSE;
}

/*
 * Update the actual drawing area metrics, which may be different from
 * requested, since there is no full control of the WM.
 */
static void
da_size_callback (GtkWidget       *drawing_area,
                  GtkAllocation   *allocation,
                  AnimationDialog *dialog)
{
  AnimationDialogPrivate  *priv = GET_PRIVATE (dialog);
  guchar                 **drawing_data;
  gint                     preview_width, preview_height;

  if (drawing_area == priv->shape_drawing_area)
    {
      if (allocation->width  == priv->shape_drawing_area_width &&
          allocation->height == priv->shape_drawing_area_height)
        return;

      priv->shape_drawing_area_width  = allocation->width;
      priv->shape_drawing_area_height = allocation->height;

      g_free (priv->shape_drawing_area_data);
      drawing_data = &priv->shape_drawing_area_data;
    }
  else
    {
      if (allocation->width  == priv->drawing_area_width &&
          allocation->height == priv->drawing_area_height)
        return;

      priv->drawing_area_width  = allocation->width;
      priv->drawing_area_height = allocation->height;

      g_free (priv->drawing_area_data);
      drawing_data = &priv->drawing_area_data;
    }

  animation_playback_get_size (priv->playback,
                               &preview_width, &preview_height);
  priv->zoom = MIN ((gdouble) allocation->width / (gdouble) preview_width,
                    (gdouble) allocation->height / (gdouble) preview_height);

  *drawing_data = g_malloc (allocation->width * allocation->height * 3);

  if (is_detached (dialog) &&
      drawing_area == priv->drawing_area)
    {
      /* Set "alpha grid" background. */
      total_alpha_preview (priv->drawing_area_data,
                           allocation->width,
                           allocation->height);
      repaint_da (priv->drawing_area, NULL, dialog);
    }
  else
    {
      /* Update the zoom information. */
      GtkEntry *zoomcombo_text_child;

      zoomcombo_text_child = GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->zoomcombo)));
      if (zoomcombo_text_child)
        {
          char* new_entry_text = g_strdup_printf  (_("%.1f %%"), priv->zoom * 100.0);

          gtk_entry_set_text (zoomcombo_text_child, new_entry_text);
          g_free (new_entry_text);
        }

      /* As we re-allocated the drawn data, let's render it again. */
      if (priv->animation && animation_loaded (priv->animation))
        {
          if (! gtk_widget_get_realized (drawing_area))
            {
              /* Render will crash if the drawing are is not realized yet.
               * So I connect a handler to render on realization. */
              g_signal_connect (drawing_area, "realize",
                                G_CALLBACK (render_on_realize),
                                dialog);
            }
          else
            {
              GeglBuffer *buffer;

              buffer = animation_playback_get_buffer (priv->playback,
                                                      animation_playback_get_position (priv->playback));
              render_frame (dialog, buffer, TRUE);
              /* clean up */
              if (buffer)
                g_object_unref (buffer);
            }
        }
    }
}

static gboolean
shape_pressed (GtkWidget       *widget,
               GdkEventButton  *event,
               AnimationDialog *dialog)
{
  if (da_button_press (widget, event, dialog))
    return TRUE;

  /* ignore double and triple click */
  if (event->type == GDK_BUTTON_PRESS)
    {
      CursorOffset *p = g_object_get_data (G_OBJECT(widget), "cursor-offset");

      if (!p)
        return FALSE;

      p->x = event->x;
      p->y = event->y;

      gtk_grab_add (widget);
      gdk_pointer_grab (gtk_widget_get_window (widget), TRUE,
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_BUTTON_MOTION_MASK  |
                        GDK_POINTER_MOTION_HINT_MASK,
                        NULL, NULL, 0);
      gdk_window_raise (gtk_widget_get_window (widget));
    }

  return FALSE;
}

static gboolean
shape_released (GtkWidget *widget)
{
  gtk_grab_remove (widget);
  gdk_display_pointer_ungrab (gtk_widget_get_display (widget), 0);
  gdk_flush ();

  return FALSE;
}

static gboolean
shape_motion (GtkWidget      *widget,
              GdkEventMotion *event)
{
  GdkModifierType  mask;
  gint             xp, yp;
  GdkWindow       *root_win;

  root_win = gdk_get_default_root_window ();
  gdk_window_get_pointer (root_win, &xp, &yp, &mask);

  /* if a button is still held by the time we process this event... */
  if (mask & GDK_BUTTON1_MASK)
    {
      CursorOffset *p = g_object_get_data (G_OBJECT (widget), "cursor-offset");

      if (!p)
        return FALSE;

      gtk_window_move (GTK_WINDOW (widget), xp  - p->x, yp  - p->y);
    }
  else /* the user has released all buttons */
    {
      shape_released (widget);
    }

  return FALSE;
}

static void
render_callback (AnimationPlayback *playback,
                 gint               frame_number,
                 GeglBuffer        *buffer,
                 gboolean           must_draw_null,
                 AnimationDialog   *dialog)
{
  render_frame (dialog, buffer, must_draw_null);

  /* Update UI. */
  show_playing_progress (dialog);
}

static void
render_on_realize (GtkWidget       *drawing_area,
                   AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GeglBuffer             *buffer;

  buffer = animation_playback_get_buffer (priv->playback,
                                          animation_playback_get_position (priv->playback));
  render_frame (dialog, buffer, TRUE);
  /* clean up */
  if (buffer)
    g_object_unref (buffer);

  g_signal_handlers_disconnect_by_func (drawing_area,
                                        G_CALLBACK (render_on_realize),
                                        dialog);
}

static void
render_frame (AnimationDialog *dialog,
              GeglBuffer      *buffer,
              gboolean         must_draw_null)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  static gchar  *shape_preview_mask      = NULL;
  static guint   shape_preview_mask_size = 0;
  static guchar *rawframe                = NULL;
  static guint   rawframe_size           = 0;
  gint           i, j, k;
  guchar        *srcptr;
  guchar        *destptr;
  GtkWidget     *da;
  guint          drawing_width;
  guint          drawing_height;
  guchar        *preview_data;
  gint           preview_width, preview_height;

  /* If the animation returns a NULL buffer, it means the image to
   * display hasn't changed. */
  if ((!must_draw_null && buffer == NULL)                ||
      ! animation_loaded (priv->animation)               ||
      /* Do not try to render on unrealized drawing areas. */
      ! gtk_widget_get_realized (priv->shape_drawing_area) ||
      ! gtk_widget_get_realized (priv->drawing_area))
    return;

  if (!priv->rendered_once)
    {
      /* Fit to display on first render. */
      update_scale (ANIMATION_DIALOG (dialog),
                    get_zoom (ANIMATION_DIALOG (dialog), 0));
    }
  priv->rendered_once = TRUE;

  if (is_detached (dialog))
    {
      da = priv->shape_drawing_area;
      preview_data = priv->shape_drawing_area_data;
      drawing_width = priv->shape_drawing_area_width;
      drawing_height = priv->shape_drawing_area_height;

      if (animation_get_duration (priv->animation) < 1)
        total_alpha_preview (preview_data, drawing_width, drawing_height);
    }
  else
    {
      da = priv->drawing_area;
      preview_data = priv->drawing_area_data;
      drawing_width = priv->drawing_area_width;
      drawing_height = priv->drawing_area_height;

      /* Set "alpha grid" background. */
      total_alpha_preview (preview_data, drawing_width, drawing_height);
    }

  /* When there is no frame to show, we simply display the alpha background and return. */
  if (buffer && animation_get_duration (priv->animation) > 0)
    {
      /* Update the rawframe. */
      if (rawframe_size < drawing_width * drawing_height * 4)
        {
          rawframe_size = drawing_width * drawing_height * 4;
          g_free (rawframe);
          rawframe = g_malloc (rawframe_size);
        }

      /* Fetch and scale the whole raw new frame */
      gegl_buffer_get (buffer, GEGL_RECTANGLE (0, 0, drawing_width, drawing_height),
                       priv->zoom, babl_format ("R'G'B'A u8"),
                       rawframe, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_CLAMP);

      /* Number of pixels. */
      i = drawing_width * drawing_height;
      destptr = preview_data;
      srcptr  = rawframe;
      while (i--)
        {
          if (! (srcptr[3] & 128))
            {
              srcptr  += 4;
              destptr += 3;
              continue;
            }

          *(destptr++) = *(srcptr++);
          *(destptr++) = *(srcptr++);
          *(destptr++) = *(srcptr++);

          srcptr++;
        }

      /* calculate the shape mask */
      if (is_detached (dialog))
        {
          gint ideal_shape_size = (drawing_width * drawing_height) /
                                   8 + 1 + drawing_height;

          if (shape_preview_mask_size < ideal_shape_size)
            {
              shape_preview_mask_size = ideal_shape_size;
              g_free (shape_preview_mask);
              shape_preview_mask = g_malloc (ideal_shape_size);
            }

          memset (shape_preview_mask, 0,
                  (drawing_width * drawing_height) / 8 + drawing_height);
          srcptr = rawframe + 3;

          for (j = 0; j < drawing_height; j++)
            {
              k = j * ((7 + drawing_width) / 8);

              for (i = 0; i < drawing_width; i++)
                {
                  if ((*srcptr) & 128)
                    shape_preview_mask[k + i/8] |= (1 << (i&7));

                  srcptr += 4;
                }
            }
          reshape_from_bitmap (dialog, shape_preview_mask);
        }
    }

  /* Display the preview buffer. */
  animation_playback_get_size (priv->playback,
                               &preview_width, &preview_height);
  gdk_draw_rgb_image (gtk_widget_get_window (da),
                      (gtk_widget_get_style (da))->white_gc,
                      (gint) (((gint)drawing_width - priv->zoom * preview_width) / 2),
                      (gint) (((gint)drawing_height - priv->zoom * preview_height) / 2),
                      (gint)drawing_width, (gint)drawing_height,
                      (animation_get_duration (priv->animation) == 1 ?
                       GDK_RGB_DITHER_MAX : DITHERTYPE),
                      preview_data, drawing_width * 3);
}

static void
reshape_from_bitmap (AnimationDialog *dialog,
                     const gchar     *bitmap)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  static gchar *prev_bitmap = NULL;
  static guint  prev_width = -1;
  static guint  prev_height = -1;

  if ((! prev_bitmap)                                  ||
      prev_width != priv->shape_drawing_area_width   ||
      prev_height != priv->shape_drawing_area_height ||
      (memcmp (prev_bitmap, bitmap,
               (priv->shape_drawing_area_width *
                priv->shape_drawing_area_height) / 8 +
               priv->shape_drawing_area_height)))
    {
      GdkBitmap *shape_mask;

      shape_mask = gdk_bitmap_create_from_data (gtk_widget_get_window (priv->shape_window),
                                                bitmap,
                                                priv->shape_drawing_area_width, priv->shape_drawing_area_height);
      gtk_widget_shape_combine_mask (priv->shape_window, shape_mask, 0, 0);
      g_object_unref (shape_mask);

      if (!prev_bitmap || prev_width != priv->shape_drawing_area_width || prev_height != priv->shape_drawing_area_height)
        {
          g_free(prev_bitmap);
          prev_bitmap = g_malloc ((priv->shape_drawing_area_width * priv->shape_drawing_area_height) / 8 + priv->shape_drawing_area_height);
          prev_width = priv->shape_drawing_area_width;
          prev_height = priv->shape_drawing_area_height;
        }

      memcpy (prev_bitmap, bitmap, (priv->shape_drawing_area_width * priv->shape_drawing_area_height) / 8 + priv->shape_drawing_area_height);
    }
}

static gboolean
on_progress_event (GtkWidget        *widget,
                   GdkEvent         *event,
                   AnimationDialog  *dialog)
{
  static gint             revert_position = -1;
  static gboolean         in_progress     = FALSE;
  AnimationDialogPrivate *priv            = GET_PRIVATE (dialog);
  gboolean                jump;
  gint                    x;

  jump = FALSE;
  switch (event->type)
    {
    case GDK_LEAVE_NOTIFY:
      in_progress = FALSE;
      break;
    case GDK_ENTER_NOTIFY:
      in_progress = TRUE;
      break;
    case GDK_MOTION_NOTIFY:
      x = ((GdkEventMotion*) event)->x;
      if (revert_position >= 0)
        jump = TRUE;
      break;
    case GDK_BUTTON_PRESS:
      x = ((GdkEventButton*) event)->x;
      revert_position = animation_playback_get_position (priv->playback);
      jump = TRUE;
      break;
    case GDK_BUTTON_RELEASE:
      x = ((GdkEventButton*) event)->x;
      if (in_progress)
        jump = TRUE;
      else
        animation_playback_jump (priv->playback, revert_position);
      revert_position = -1;
      break;
    default:
      /* Should not happen. */
      return FALSE;
    }

  if (jump)
    {
      GtkAllocation  allocation;
      gdouble        duration;
      gint           frame;

      gtk_widget_get_allocation (widget, &allocation);
      duration = (gdouble) animation_get_duration (priv->animation);

      frame = (gint) (x / ((gdouble) allocation.width /
                           ((gdouble) duration - 0.99)));

      animation_playback_jump (priv->playback, frame);
    }

  return FALSE;
}

static void
show_playing_progress (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gchar                  *text;
  gdouble                 framerate;
  gint                    position;

  framerate = animation_get_framerate (priv->animation);
  position = animation_playback_get_position (priv->playback),

  /* update the dialog's progress bar */
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (priv->progress),
                                 ((gfloat) animation_playback_get_position (priv->playback) /
                                  (gfloat) (animation_get_duration (priv->animation) - 0.999)));

  text = g_strdup_printf (_("Frame: %d/%d - Time: %.2f s"),
                          position + 1,
                          animation_get_duration (priv->animation),
                          position / framerate);

  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (priv->progress), text);
  g_free (text);
}

static void
inactive_on_loading (Animation *animation,
                     gdouble    load_rate,
                     GtkWidget *widget)
{
  gtk_widget_set_sensitive (widget, FALSE);
}

static void
active_on_loaded (Animation *animation,
                  GtkWidget *widget)
{
  gtk_widget_set_sensitive (widget, TRUE);
}

static void
hide_on_animatic (AnimationDialog *dialog,
                  GParamSpec      *param_spec,
                  GtkWidget       *widget)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (! priv->animation || ANIMATION_IS_ANIMATIC (priv->animation))
    gtk_widget_hide (widget);
  else
    gtk_widget_show (widget);
}

static void
update_progress (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  gint frame_spin_size;
  gint duration = animation_get_duration (priv->animation);
  gint playback_start;
  gint playback_stop;

  playback_start = animation_playback_get_start (priv->playback);
  playback_stop = animation_playback_get_stop (priv->playback);

  frame_spin_size = (gint) (log10 (duration + 1 - (duration % 10))) + 1;
  gtk_entry_set_width_chars (GTK_ENTRY (priv->startframe_spin), frame_spin_size);
  gtk_entry_set_width_chars (GTK_ENTRY (priv->endframe_spin), frame_spin_size);

  gtk_adjustment_configure (gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (priv->startframe_spin)),
                            (gdouble) playback_start + 1.0,
                            1.0,
                            (gdouble) duration,
                            1.0, 5.0, 0.0);
  gtk_adjustment_configure (gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (priv->endframe_spin)),
                            (gdouble) playback_stop + 1.0,
                            (gdouble) playback_start + 1.0,
                            (gdouble) duration,
                            1.0, 5.0, 0.0);

  update_ui_sensitivity (dialog);
}

static void
block_ui (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  if (animation_playback_is_playing (priv->playback))
    play_pause (dialog);

  gtk_action_group_set_sensitive (priv->play_actions, FALSE);
  gtk_widget_set_sensitive (priv->play_bar, FALSE);
  gtk_widget_set_sensitive (priv->progress_bar, FALSE);
  gtk_action_group_set_sensitive (priv->settings_actions, FALSE);
  gtk_action_group_set_sensitive (priv->view_actions, FALSE);
}

static gboolean
is_detached (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  return (gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (gtk_action_group_get_action (priv->view_actions, "detach"))));
}

static void
play_pause (AnimationDialog *dialog)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

  gtk_action_activate (gtk_action_group_get_action (priv->play_actions, "play"));
}

/* get_fps:
 * Frame rate proposed as default.
 * These are common framerates.
 */
static gdouble
get_fps (gint       index)
{
  gdouble fps;

  switch (index)
    {
    case 0:
      fps = 12.0;
      break;
    case 1:
      fps = 24.0;
      break;
    case 2:
      fps = 25.0;
      break;
    case 3:
      fps = 30.0;
      break;
    case 4:
      fps = 48.0;
      break;
    default:
      fps = 24.0;
      break;
    }

  return fps;
}

/* Returns the zoom selected in the UI. */
static gdouble
get_zoom (AnimationDialog *dialog,
          gint             index)
{
  switch (index)
    {
    case 0:
      /* Fit to the scrolled area! */
        {
          AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
          gdouble                 zoom = 1.0;

          if (priv->animation)
            {
              GtkAllocation allocation;
              gint          width;
              gint          height;

              animation_playback_get_size (priv->playback,
                                           &width, &height);
              gtk_widget_get_allocation (priv->scrolled_drawing_area, &allocation);
              if (width > allocation.width || height > allocation.height)
                {
                  zoom = MIN ((gdouble) allocation.width / (gdouble) width,
                              (gdouble) allocation.height / (gdouble) height);
                }
            }

          return zoom;
        }
    case 1:
      return 0.51;
    case 2:
      return 1.0;
    case 3:
      return 1.25;
    case 4:
      return 1.5;
    case 5:
      return 2.0;
    default:
      {
        AnimationDialogPrivate *priv = GET_PRIVATE (dialog);

        /* likely -1 returned if there is no active item from the list.
         * Try a text conversion, locale-aware in such a case, assuming people write in percent. */
        gchar   *active_text = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (priv->zoomcombo));
        gdouble  zoom        = g_strtod (active_text, NULL);
        g_free (active_text);

        /* Negative scales are inconsistent. And we want to avoid huge scaling. */
        if (zoom > 300.0)
          zoom = 300.0;

        return zoom / 100.0;
      }
    }
}

static void
update_scale (AnimationDialog *dialog,
              gdouble          scale)
{
  AnimationDialogPrivate *priv = GET_PRIVATE (dialog);
  GtkEntry               *zoomcombo_text_child;
  guint                   expected_drawing_area_width;
  guint                   expected_drawing_area_height;
  gint                    preview_width, preview_height;

  /* Replace the text with actual (expected) zoom value.
   * In particular if the user hits "Fit to display" or wrote
   * free-form text. */
  zoomcombo_text_child = GTK_ENTRY (gtk_bin_get_child (GTK_BIN (priv->zoomcombo)));
  if (zoomcombo_text_child)
    {
      char* new_entry_text = g_strdup_printf  (_("%.1f %%"), scale * 100.0);

      gtk_entry_set_text (zoomcombo_text_child, new_entry_text);
      g_free (new_entry_text);
    }

  if (priv->animation == NULL)
    return;

  animation_playback_get_size (priv->playback,
                               &preview_width, &preview_height);
  expected_drawing_area_width  = preview_width * scale;
  expected_drawing_area_height = preview_height * scale;

  /* We don't update the scale directly because this might
   * end up not being the real scale. Instead we request this size for
   * the drawing areas, and the actual scale update will be done on the
   * callback when size is actually allocated. */
  gtk_widget_set_size_request (priv->drawing_area, expected_drawing_area_width, expected_drawing_area_height);
  gtk_widget_set_size_request (priv->shape_drawing_area, expected_drawing_area_width, expected_drawing_area_height);

  /* I force the shape window to a smaller size if we scale down. */
  if (is_detached (dialog))
    {
      gint x, y;

      gdk_window_get_origin (gtk_widget_get_window (priv->shape_window), &x, &y);
      gtk_window_reshow_with_initial_size (GTK_WINDOW (priv->shape_window));
      gtk_window_move (GTK_WINDOW (priv->shape_window), x, y);
    }
}
