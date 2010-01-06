/*
 * mx-entry.c: Plain entry actor
 *
 * Copyright 2008, 2009 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Written by: Thomas Wood <thomas.wood@intel.com>
 *
 */

/**
 * SECTION:mx-entry
 * @short_description: Widget for displaying text
 *
 * #MxEntry is a simple widget for displaying text. It derives from
 * #MxWidget to add extra style and placement functionality over
 * #ClutterText. The internal #ClutterText is publicly accessibly to allow
 * applications to set further properties.
 *
 * #MxEntry supports the following pseudo style states:
 * <itemizedlist>
 *  <listitem>
 *   <para>focus: the widget has focus</para>
 *  </listitem>
 *  <listitem>
 *   <para>indeterminate: the widget is showing the hint text</para>
 *  </listitem>
 * </itemizedlist>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <clutter/clutter.h>
#ifdef HAVE_CLUTTER_IMCONTEXT
#include <clutter-imcontext/clutter-imtext.h>
#endif

#include "mx-entry.h"

#include "mx-widget.h"
#include "mx-stylable.h"
#include "mx-texture-cache.h"
#include "mx-marshal.h"
#include "mx-clipboard.h"

/* for pointer cursor support */
#include <clutter/x11/clutter-x11.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>

#define HAS_FOCUS(actor) (clutter_actor_get_stage (actor) && clutter_stage_get_key_focus ((ClutterStage *) clutter_actor_get_stage (actor)) == actor)


/* properties */
enum
{
  PROP_0,

  PROP_ENTRY,
  PROP_HINT
};

/* signals */
enum
{
  PRIMARY_ICON_CLICKED,
  SECONDARY_ICON_CLICKED,

  LAST_SIGNAL
};

#define MX_ENTRY_GET_PRIVATE(obj)     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), MX_TYPE_ENTRY, MxEntryPrivate))
#define MX_ENTRY_PRIV(x) ((MxEntry *) x)->priv


struct _MxEntryPrivate
{
  ClutterActor *entry;
  gchar        *hint;

  ClutterActor *primary_icon;
  ClutterActor *secondary_icon;

  gfloat        spacing;

  gboolean hint_visible;
};

static guint entry_signals[LAST_SIGNAL] = { 0, };

static void mx_stylable_iface_init (MxStylableIface *iface);

G_DEFINE_TYPE_WITH_CODE (MxEntry, mx_entry, MX_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (MX_TYPE_STYLABLE,
                                                mx_stylable_iface_init));

static void
mx_entry_set_property (GObject      *gobject,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  MxEntry *entry = MX_ENTRY (gobject);

  switch (prop_id)
    {
    case PROP_ENTRY:
      mx_entry_set_text (entry, g_value_get_string (value));
      break;

    case PROP_HINT:
      mx_entry_set_hint_text (entry, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
mx_entry_get_property (GObject    *gobject,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (gobject);

  switch (prop_id)
    {
    case PROP_ENTRY:
      g_value_set_string (value, clutter_text_get_text (CLUTTER_TEXT (priv->entry)));
      break;

    case PROP_HINT:
      g_value_set_string (value, priv->hint);

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
mx_entry_dispose (GObject *object)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (object);

  if (priv->entry)
    {
      clutter_actor_unparent (priv->entry);
      priv->entry = NULL;
    }

  G_OBJECT_CLASS (mx_entry_parent_class)->dispose (object);
}

static void
mx_entry_finalize (GObject *object)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (object);

  g_free (priv->hint);
  priv->hint = NULL;

  G_OBJECT_CLASS (mx_entry_parent_class)->finalize (object);
}

static void
mx_stylable_iface_init (MxStylableIface *iface)
{
  static gboolean is_initialized = FALSE;

  if (!is_initialized)
    {
      GParamSpec *pspec;
      static const ClutterColor default_color
        = { 0x0, 0x9c, 0xcf, 0xff };

      is_initialized = TRUE;

      pspec = clutter_param_spec_color ("caret-color",
                                        "Caret Color",
                                        "Color of the entry's caret",
                                        &default_color,
                                        G_PARAM_READWRITE);
      mx_stylable_iface_install_property (iface, MX_TYPE_ENTRY, pspec);

      pspec = clutter_param_spec_color ("selection-background-color",
                                        "Selection Background Color",
                                        "Color of the entry's selection",
                                        &default_color,
                                        G_PARAM_READWRITE);
      mx_stylable_iface_install_property (iface, MX_TYPE_ENTRY, pspec);
    }
}

static void
mx_entry_style_changed (MxWidget *self)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (self);
  ClutterColor *color = NULL;
  ClutterColor *caret_color = NULL;
  ClutterColor *selection_background_color = NULL;
  gchar *font_name;
  gchar *font_string;
  gint font_size;

  mx_stylable_get (MX_STYLABLE (self),
                   "color", &color,
                   "caret-color", &caret_color,
                   "selection-background-color", &selection_background_color,
                   "font-family", &font_name,
                   "font-size", &font_size,
                   NULL);

  if (color)
    {
      clutter_text_set_color (CLUTTER_TEXT (priv->entry), color);
      clutter_color_free (color);
    }

  if (caret_color)
    {
      clutter_text_set_cursor_color (CLUTTER_TEXT (priv->entry), caret_color);
      clutter_color_free (caret_color);
    }

  if (selection_background_color)
    {
      clutter_text_set_selection_color (CLUTTER_TEXT (priv->entry),
                                        selection_background_color);
      clutter_color_free (selection_background_color);
    }

  if (font_name || font_size)
    {
      if (font_name && font_size)
        {
          font_string = g_strdup_printf ("%s %dpx", font_name, font_size);
          g_free (font_name);
        }
      else
        {
          if (font_size)
            font_string = g_strdup_printf ("%dpx", font_size);
          else
            font_string = font_name;
        }

      clutter_text_set_font_name (CLUTTER_TEXT (priv->entry), font_string);
      g_free (font_string);
    }
}

static void
mx_entry_get_preferred_width (ClutterActor *actor,
                              gfloat        for_height,
                              gfloat       *min_width_p,
                              gfloat       *natural_width_p)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);
  MxPadding padding;
  gfloat icon_w;

  mx_widget_get_padding (MX_WIDGET (actor), &padding);

  for_height -= padding.top + padding.bottom;

  clutter_actor_get_preferred_width (priv->entry, for_height,
                                     min_width_p,
                                     natural_width_p);

  if (priv->primary_icon)
    {
      clutter_actor_get_preferred_width (priv->primary_icon, -1, NULL, &icon_w);

      if (min_width_p)
        *min_width_p += icon_w + priv->spacing;

      if (natural_width_p)
        *natural_width_p += icon_w + priv->spacing;
    }

  if (priv->secondary_icon)
    {
      clutter_actor_get_preferred_width (priv->secondary_icon,
                                         -1, NULL, &icon_w);

      if (min_width_p)
        *min_width_p += icon_w + priv->spacing;

      if (natural_width_p)
        *natural_width_p += icon_w + priv->spacing;
    }

  if (min_width_p)
    *min_width_p += padding.left + padding.right;

  if (natural_width_p)
    *natural_width_p += padding.left + padding.right;
}

static void
mx_entry_get_preferred_height (ClutterActor *actor,
                               gfloat        for_width,
                               gfloat       *min_height_p,
                               gfloat       *natural_height_p)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);
  MxPadding padding;
  gfloat icon_h;

  mx_widget_get_padding (MX_WIDGET (actor), &padding);

  for_width -= padding.left + padding.right;

  clutter_actor_get_preferred_height (priv->entry, for_width,
                                      min_height_p,
                                      natural_height_p);

  if (priv->primary_icon)
    {
      clutter_actor_get_preferred_height (priv->primary_icon,
                                          -1, NULL, &icon_h);

      if (min_height_p && icon_h > *min_height_p)
        *min_height_p = icon_h;

      if (natural_height_p && icon_h > *natural_height_p)
        *natural_height_p = icon_h;
    }

  if (priv->secondary_icon)
    {
      clutter_actor_get_preferred_height (priv->secondary_icon,
                                          -1, NULL, &icon_h);

      if (min_height_p && icon_h > *min_height_p)
        *min_height_p = icon_h;

      if (natural_height_p && icon_h > *natural_height_p)
        *natural_height_p = icon_h;
    }

  if (min_height_p)
    *min_height_p += padding.top + padding.bottom;

  if (natural_height_p)
    *natural_height_p += padding.top + padding.bottom;
}

static void
mx_entry_allocate (ClutterActor          *actor,
                   const ClutterActorBox *box,
                   ClutterAllocationFlags flags)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);
  ClutterActorClass *parent_class;
  ClutterActorBox child_box, icon_box;
  MxPadding padding;
  gfloat icon_w, icon_h;
  gfloat entry_h, min_h, pref_h, avail_h;

  mx_widget_get_padding (MX_WIDGET (actor), &padding);

  parent_class = CLUTTER_ACTOR_CLASS (mx_entry_parent_class);
  parent_class->allocate (actor, box, flags);

  avail_h = (box->y2 - box->y1) - padding.top - padding.bottom;

  child_box.x1 = padding.left;
  child_box.x2 = box->x2 - box->x1 - padding.right;

  if (priv->primary_icon)
    {
      clutter_actor_get_preferred_width (priv->primary_icon,
                                         -1, NULL, &icon_w);
      clutter_actor_get_preferred_height (priv->primary_icon,
                                          -1, NULL, &icon_h);

      icon_box.x1 = padding.left;
      icon_box.x2 = icon_box.x1 + icon_w;

      icon_box.y1 = (int)(padding.top + avail_h / 2 - icon_h / 2);
      icon_box.y2 = icon_box.y1 + icon_h;

      clutter_actor_allocate (priv->primary_icon,
                              &icon_box,
                              flags);

      /* reduce the size for the entry */
      child_box.x1 += icon_w + priv->spacing;
    }

  if (priv->secondary_icon)
    {
      clutter_actor_get_preferred_width (priv->secondary_icon,
                                         -1, NULL, &icon_w);
      clutter_actor_get_preferred_height (priv->secondary_icon,
                                          -1, NULL, &icon_h);

      icon_box.x2 = (box->x2 - box->x1) - padding.right;
      icon_box.x1 = icon_box.x2 - icon_w;

      icon_box.y1 = (int)(padding.top + avail_h / 2 - icon_h / 2);
      icon_box.y2 = icon_box.y1 + icon_h;

      clutter_actor_allocate (priv->secondary_icon,
                              &icon_box,
                              flags);

      /* reduce the size for the entry */
      child_box.x2 -= icon_w - priv->spacing;
    }

  clutter_actor_get_preferred_height (priv->entry, child_box.x2 - child_box.x1,
                                      &min_h, &pref_h);

  entry_h = CLAMP (pref_h, min_h, avail_h);

  child_box.y1 = (int)(padding.top + avail_h / 2 - entry_h / 2);
  child_box.y2 = child_box.y1 + entry_h;

  clutter_actor_allocate (priv->entry, &child_box, flags);
}

static void
clutter_text_focus_in_cb (ClutterText  *text,
                          ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);

  /* remove the hint if visible */
  if (priv->hint && priv->hint_visible)
    {
      priv->hint_visible = FALSE;

      clutter_text_set_text (text, "");
    }
  mx_stylable_set_style_pseudo_class (MX_STYLABLE (actor), "focus");
  clutter_text_set_cursor_visible (text, TRUE);
}

static void
clutter_text_focus_out_cb (ClutterText  *text,
                           ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);

  /* add a hint if the entry is empty */
  if (priv->hint && !strcmp (clutter_text_get_text (text), ""))
    {
      priv->hint_visible = TRUE;

      clutter_text_set_text (text, priv->hint);
      mx_stylable_set_style_pseudo_class (MX_STYLABLE (actor), "indeterminate");
    }
  else
    {
      mx_stylable_set_style_pseudo_class (MX_STYLABLE (actor), NULL);
    }
  clutter_text_set_cursor_visible (text, FALSE);
}

static void
mx_entry_paint (ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);
  ClutterActorClass *parent_class;

  parent_class = CLUTTER_ACTOR_CLASS (mx_entry_parent_class);
  parent_class->paint (actor);

  clutter_actor_paint (priv->entry);

  if (priv->primary_icon)
    clutter_actor_paint (priv->primary_icon);

  if (priv->secondary_icon)
    clutter_actor_paint (priv->secondary_icon);
}

static void
mx_entry_pick (ClutterActor       *actor,
               const ClutterColor *c)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);

  CLUTTER_ACTOR_CLASS (mx_entry_parent_class)->pick (actor, c);

  clutter_actor_paint (priv->entry);

  if (priv->primary_icon)
    clutter_actor_paint (priv->primary_icon);

  if (priv->secondary_icon)
    clutter_actor_paint (priv->secondary_icon);
}

static void
mx_entry_map (ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY (actor)->priv;

  CLUTTER_ACTOR_CLASS (mx_entry_parent_class)->map (actor);

  clutter_actor_map (priv->entry);

  if (priv->primary_icon)
    clutter_actor_map (priv->primary_icon);

  if (priv->secondary_icon)
    clutter_actor_map (priv->secondary_icon);
}

static void
mx_entry_unmap (ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY (actor)->priv;

  CLUTTER_ACTOR_CLASS (mx_entry_parent_class)->unmap (actor);

  clutter_actor_unmap (priv->entry);

  if (priv->primary_icon)
    clutter_actor_unmap (priv->primary_icon);

  if (priv->secondary_icon)
    clutter_actor_unmap (priv->secondary_icon);
}

static void
mx_entry_clipboard_callback (MxClipboard *clipboard,
                             const gchar *text,
                             gpointer     data)
{
  ClutterText *ctext = (ClutterText*)((MxEntry *) data)->priv->entry;
  gint cursor_pos;

  if (!text)
    return;

  /* delete the current selection before pasting */
  clutter_text_delete_selection (ctext);

  /* "paste" the clipboard text into the entry */
  cursor_pos = clutter_text_get_cursor_position (ctext);
  clutter_text_insert_text (ctext, text, cursor_pos);
}

static gboolean
mx_entry_key_press_event (ClutterActor    *actor,
                          ClutterKeyEvent *event)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);

  /* This is expected to handle events that were emitted for the inner
     ClutterText. They only reach this function if the ClutterText
     didn't handle them */

  /* paste */
  if ((event->modifier_state & CLUTTER_CONTROL_MASK)
      && event->keyval == CLUTTER_v)
    {
      MxClipboard *clipboard;

      clipboard = mx_clipboard_get_default ();

      mx_clipboard_get_text (clipboard, mx_entry_clipboard_callback, actor);

      return TRUE;
    }

  /* copy */
  if ((event->modifier_state & CLUTTER_CONTROL_MASK)
      && event->keyval == CLUTTER_c)
    {
      MxClipboard *clipboard;
      gchar *text;

      clipboard = mx_clipboard_get_default ();

      text = clutter_text_get_selection ((ClutterText*) priv->entry);

      if (text && strlen (text))
        mx_clipboard_set_text (clipboard, text);

      return TRUE;
    }


  /* cut */
  if ((event->modifier_state & CLUTTER_CONTROL_MASK)
      && event->keyval == CLUTTER_x)
    {
      MxClipboard *clipboard;
      gchar *text;

      clipboard = mx_clipboard_get_default ();

      text = clutter_text_get_selection ((ClutterText*) priv->entry);

      if (text && strlen (text))
        {
          mx_clipboard_set_text (clipboard, text);

          /* now delete the text */
          clutter_text_delete_selection ((ClutterText *) priv->entry);
        }

      return TRUE;
    }

  return FALSE;
}

static void
mx_entry_key_focus_in (ClutterActor *actor)
{
  MxEntryPrivate *priv = MX_ENTRY_PRIV (actor);

  /* We never want key focus. The ClutterText should be given first
     pass for all key events */
  clutter_actor_grab_key_focus (priv->entry);
}

static gboolean
mx_entry_swallow_crossing_event (ClutterActor         *actor,
                                 ClutterCrossingEvent *event)
{

  if (event->source == MX_ENTRY (actor)->priv->entry
      && event->related != NULL)
    {
      Display *dpy;
      ClutterActor *stage;
      Window wid;
      static Cursor ibeam = None;

      dpy = clutter_x11_get_default_display ();
      stage = clutter_actor_get_stage (actor);
      wid = clutter_x11_get_stage_window (CLUTTER_STAGE (stage));

      if (ibeam == None)
        ibeam = XCreateFontCursor (dpy, XC_xterm);

      if (event->type == CLUTTER_ENTER)
        XDefineCursor (dpy, wid, ibeam);
      else
        XUndefineCursor (dpy, wid);
    }

  /* swallow enter and leave events, since the pseudo-class must not be set to
   * 'hover' because it would loose the 'focus' state.
   */
  return TRUE;
}

static gboolean
mx_entry_swallow_button_event (ClutterActor       *actor,
                               ClutterButtonEvent *event)
{
  /* swallow button events, since the pseudo-class must not be set to
   * 'active' because it would loose the 'focus' state.
   */

  return TRUE;
}

static void
mx_entry_class_init (MxEntryClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (MxEntryPrivate));

  gobject_class->set_property = mx_entry_set_property;
  gobject_class->get_property = mx_entry_get_property;
  gobject_class->finalize = mx_entry_finalize;
  gobject_class->dispose = mx_entry_dispose;

  actor_class->get_preferred_width = mx_entry_get_preferred_width;
  actor_class->get_preferred_height = mx_entry_get_preferred_height;
  actor_class->allocate = mx_entry_allocate;
  actor_class->paint = mx_entry_paint;
  actor_class->pick = mx_entry_pick;
  actor_class->map = mx_entry_map;
  actor_class->unmap = mx_entry_unmap;
  actor_class->enter_event = mx_entry_swallow_crossing_event;
  actor_class->leave_event = mx_entry_swallow_crossing_event;
  actor_class->button_press_event = mx_entry_swallow_button_event;
  actor_class->button_release_event = mx_entry_swallow_button_event;

  actor_class->key_press_event = mx_entry_key_press_event;
  actor_class->key_focus_in = mx_entry_key_focus_in;

  pspec = g_param_spec_string ("text",
                               "Text",
                               "Text of the entry",
                               NULL, G_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_ENTRY, pspec);

  pspec = g_param_spec_string ("hint-text",
                               "Hint Text",
                               "Text to display when the entry is not focused "
                               "and the text property is empty",
                               NULL, G_PARAM_READWRITE);
  g_object_class_install_property (gobject_class, PROP_ENTRY, pspec);

  /* signals */
  /**
   * MxEntry::primary-icon-clicked:
   *
   * Emitted when the primary icon is clicked
   */
  entry_signals[PRIMARY_ICON_CLICKED] =
    g_signal_new ("primary-icon-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (MxEntryClass, primary_icon_clicked),
                  NULL, NULL,
                  _mx_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
  /**
   * MxEntry::secondary-icon-clicked:
   *
   * Emitted when the secondary icon is clicked
   */
  entry_signals[SECONDARY_ICON_CLICKED] =
    g_signal_new ("secondary-icon-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (MxEntryClass, secondary_icon_clicked),
                  NULL, NULL,
                  _mx_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

static void
mx_entry_init (MxEntry *entry)
{
  MxEntryPrivate *priv;

  priv = entry->priv = MX_ENTRY_GET_PRIVATE (entry);

#ifdef HAVE_CLUTTER_IM_CONTEXT
  priv->entry = g_object_new (CLUTTER_TYPE_IMTEXT,
#else
  priv->entry = g_object_new (CLUTTER_TYPE_TEXT,
#endif
                              "line-alignment", PANGO_ALIGN_LEFT,
                              "editable", TRUE,
                              "reactive", TRUE,
                              "single-line-mode", TRUE,
                              NULL);

  g_signal_connect (priv->entry, "key-focus-in",
                    G_CALLBACK (clutter_text_focus_in_cb), entry);

  g_signal_connect (priv->entry, "key-focus-out",
                    G_CALLBACK (clutter_text_focus_out_cb), entry);

  priv->spacing = 6.0f;

  clutter_actor_set_parent (priv->entry, CLUTTER_ACTOR (entry));
  clutter_actor_set_reactive ((ClutterActor *) entry, TRUE);

  /* set cursor hidden until we receive focus */
  clutter_text_set_cursor_visible ((ClutterText *) priv->entry, FALSE);

  g_signal_connect (entry, "style-changed",
                    G_CALLBACK (mx_entry_style_changed), NULL);
}

/**
 * mx_entry_new:
 * @text: text to set the entry to
 *
 * Create a new #MxEntry with the specified entry
 *
 * Returns: a new #MxEntry
 */
ClutterActor *
mx_entry_new (const gchar *text)
{
  MxWidget *entry;

  /* add the entry to the stage, but don't allow it to be visible */
  entry = g_object_new (MX_TYPE_ENTRY,
                        "text", text,
                        NULL);

  return (ClutterActor*) entry;
}

/**
 * mx_entry_get_text:
 * @entry: a #MxEntry
 *
 * Get the text displayed on the entry
 *
 * Returns: the text for the entry. This must not be freed by the application
 */
G_CONST_RETURN gchar *
mx_entry_get_text (MxEntry *entry)
{
  g_return_val_if_fail (MX_IS_ENTRY (entry), NULL);

  if (entry->priv->hint_visible)
    return "";
  else
    return clutter_text_get_text (CLUTTER_TEXT (entry->priv->entry));
}

/**
 * mx_entry_set_text:
 * @entry: a #MxEntry
 * @text: text to set the entry to
 *
 * Sets the text displayed on the entry
 */
void
mx_entry_set_text (MxEntry     *entry,
                   const gchar *text)
{
  MxEntryPrivate *priv;

  g_return_if_fail (MX_IS_ENTRY (entry));

  priv = entry->priv;

  /* set a hint if we are blanking the entry */
  if (priv->hint
      && text && !strcmp ("", text)
      && !HAS_FOCUS (priv->entry))
    {
      text = priv->hint;
      priv->hint_visible = TRUE;
      mx_stylable_set_style_pseudo_class (MX_STYLABLE (entry), "indeterminate");
    }
  else
    {
      if (HAS_FOCUS (priv->entry))
        mx_stylable_set_style_pseudo_class (MX_STYLABLE (entry), "focus");
      else
        mx_stylable_set_style_pseudo_class (MX_STYLABLE (entry), NULL);

      priv->hint_visible = FALSE;
    }

  clutter_text_set_text (CLUTTER_TEXT (priv->entry), text);

  g_object_notify (G_OBJECT (entry), "text");
}

/**
 * mx_entry_get_clutter_text:
 * @entry: a #MxEntry
 *
 * Retrieve the internal #ClutterText so that extra parameters can be set
 *
 * Returns: the #ClutterText used by #MxEntry. The entry is owned by the
 * #MxEntry and should not be unref'ed by the application.
 */
ClutterActor*
mx_entry_get_clutter_text (MxEntry *entry)
{
  g_return_val_if_fail (MX_IS_ENTRY (entry), NULL);

  return entry->priv->entry;
}

/**
 * mx_entry_set_hint_text:
 * @entry: a #MxEntry
 * @text: text to set as the entry hint
 *
 * Sets the text to display when the entry is empty and unfocused. When the
 * entry is displaying the hint, it has a pseudo class of "indeterminate".
 * A value of NULL unsets the hint.
 */
void
mx_entry_set_hint_text (MxEntry     *entry,
                        const gchar *text)
{
  MxEntryPrivate *priv;

  g_return_if_fail (MX_IS_ENTRY (entry));

  priv = entry->priv;

  g_free (priv->hint);

  priv->hint = g_strdup (text);

  if (!strcmp (clutter_text_get_text (CLUTTER_TEXT (priv->entry)), "")
      && !HAS_FOCUS (priv->entry))
    {
      priv->hint_visible = TRUE;

      clutter_text_set_text (CLUTTER_TEXT (priv->entry), priv->hint);
      mx_stylable_set_style_pseudo_class (MX_STYLABLE (entry), "indeterminate");
    }
}

/**
 * mx_entry_get_hint_text:
 * @entry: a #MxEntry
 *
 * Gets the text that is displayed when the entry is empty and unfocused
 *
 * Returns: the current value of the hint property. This string is owned by the
 * #MxEntry and should not be freed or modified.
 */
G_CONST_RETURN
gchar *
mx_entry_get_hint_text (MxEntry *entry)
{
  g_return_val_if_fail (MX_IS_ENTRY (entry), NULL);

  return entry->priv->hint;
}

static gboolean
_mx_entry_icon_press_cb (ClutterActor       *actor,
                         ClutterButtonEvent *event,
                         MxEntry            *entry)
{
  MxEntryPrivate *priv = entry->priv;

  if (actor == priv->primary_icon)
    g_signal_emit (entry, entry_signals[PRIMARY_ICON_CLICKED], 0);
  else
    g_signal_emit (entry, entry_signals[SECONDARY_ICON_CLICKED], 0);

  return FALSE;
}

static void
_mx_entry_set_icon_from_file (MxEntry       *entry,
                              ClutterActor **icon,
                              const gchar   *filename)
{
  if (*icon)
    {
      g_signal_handlers_disconnect_by_func (*icon,
                                            _mx_entry_icon_press_cb,
                                            entry);
      clutter_actor_unparent (*icon);
      *icon = NULL;
    }

  if (filename)
    {
      MxTextureCache *cache;

      cache = mx_texture_cache_get_default ();



      *icon = (ClutterActor*) mx_texture_cache_get_texture (cache, filename);

      if (!*icon)
          return;

      clutter_actor_set_reactive (*icon, TRUE);
      clutter_actor_set_parent (*icon, CLUTTER_ACTOR (entry));
      g_signal_connect (*icon, "button-release-event",
                        G_CALLBACK (_mx_entry_icon_press_cb), entry);
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (entry));
}

/**
 * mx_entry_set_primary_icon_from_file:
 * @entry: a #MxEntry
 * @filename: filename of an icon
 *
 * Set the primary icon of the entry to the given filename
 */
void
mx_entry_set_primary_icon_from_file (MxEntry     *entry,
                                     const gchar *filename)
{
  MxEntryPrivate *priv;

  g_return_if_fail (MX_IS_ENTRY (entry));

  priv = entry->priv;

  _mx_entry_set_icon_from_file (entry, &priv->primary_icon, filename);

}

/**
 * mx_entry_set_secondary_icon_from_file:
 * @entry: a #MxEntry
 * @filename: filename of an icon
 *
 * Set the primary icon of the entry to the given filename
 */
void
mx_entry_set_secondary_icon_from_file (MxEntry     *entry,
                                       const gchar *filename)
{
  MxEntryPrivate *priv;

  g_return_if_fail (MX_IS_ENTRY (entry));

  priv = entry->priv;

  _mx_entry_set_icon_from_file (entry, &priv->secondary_icon, filename);

}

