/* GStreamer
 * Copyright (C) 2004 Wim Taymans <wim@fluendo.com>
 *
 * gstoggdemux.c: ogg stream demuxer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gst/gst.h>
#include <gst/bytestream/filepad.h>
#include <ogg/ogg.h>
#include <string.h>

#define CHUNKSIZE (8500)        /* this is out of vorbisfile */

enum
{
  OV_EREAD = -1,
  OV_EFAULT = -2,
  OV_FALSE = -3,
  OV_EOF = -4,
};

GST_DEBUG_CATEGORY_STATIC (gst_ogg_demux_debug);
GST_DEBUG_CATEGORY_STATIC (gst_ogg_demux_setup_debug);
#define GST_CAT_DEFAULT gst_ogg_demux_debug

#define GST_TYPE_OGG_PAD (gst_ogg_pad_get_type())
#define GST_OGG_PAD(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OGG_PAD, GstOggPad))
#define GST_OGG_PAD_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OGG_PAD, GstOggPad))
#define GST_IS_OGG_PAD(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OGG_PAD))
#define GST_IS_OGG_PAD_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OGG_PAD))

typedef struct _GstOggPad GstOggPad;
typedef struct _GstOggPadClass GstOggPadClass;

#define GST_TYPE_OGG_DEMUX (gst_ogg_demux_get_type())
#define GST_OGG_DEMUX(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OGG_DEMUX, GstOggDemux))
#define GST_OGG_DEMUX_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OGG_DEMUX, GstOggDemux))
#define GST_IS_OGG_DEMUX(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OGG_DEMUX))
#define GST_IS_OGG_DEMUX_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OGG_DEMUX))

static GType gst_ogg_demux_get_type (void);

typedef struct _GstOggDemux GstOggDemux;
typedef struct _GstOggDemuxClass GstOggDemuxClass;

/* all information needed for one ogg stream */
struct _GstOggPad
{
  GstRealPad pad;               /* subclass GstRealPad */

  GstOggDemux *ogg;

  gint serialno;
  gint64 packetno;
  gint64 offset;

  gint64 current_granule;
  gint64 first_granule;
  gint64 last_granule;

  ogg_stream_state stream;
};

struct _GstOggPadClass
{
  GstRealPadClass parent_class;
};

static void gst_ogg_pad_init (GstOggPad * pad);
static const GstFormat *gst_ogg_pad_formats (GstPad * pad);
static const GstEventMask *gst_ogg_pad_event_masks (GstPad * pad);
static const GstQueryType *gst_ogg_pad_query_types (GstPad * pad);
static gboolean gst_ogg_pad_src_convert (GstPad * pad,
    GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value);
static gboolean gst_ogg_pad_src_query (GstPad * pad, GstQueryType type,
    GstFormat * format, gint64 * value);
static GstCaps *gst_ogg_type_find (ogg_packet * packet);

static GType
gst_ogg_pad_get_type (void)
{
  static GType ogg_pad_type = 0;

  if (!ogg_pad_type) {
    static const GTypeInfo ogg_pad_info = {
      sizeof (GstOggPadClass),
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      sizeof (GstOggPad),
      0,
      (GInstanceInitFunc) gst_ogg_pad_init,
    };

    ogg_pad_type =
        g_type_register_static (GST_TYPE_REAL_PAD, "GstOggPad", &ogg_pad_info,
        0);
  }
  return ogg_pad_type;
}

static void
gst_ogg_pad_init (GstOggPad * pad)
{
  //gst_pad_set_event_function (GST_PAD (pad), GST_DEBUG_FUNCPTR (gst_ogg_pad_event));
  gst_pad_set_event_mask_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_ogg_pad_event_masks));
  //gst_pad_set_getcaps_function (GST_PAD (pad), GST_DEBUG_FUNCPTR (gst_ogg_pad_getcaps));
  //gst_pad_set_query_function (GST_PAD (pad), GST_DEBUG_FUNCPTR (gst_ogg_pad_query));
  gst_pad_set_query_type_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_ogg_pad_query_types));
  gst_pad_set_formats_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_ogg_pad_formats));
  gst_pad_set_convert_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_ogg_pad_src_convert));
  gst_pad_set_query_function (GST_PAD (pad),
      GST_DEBUG_FUNCPTR (gst_ogg_pad_src_query));
}

static const GstFormat *
gst_ogg_pad_formats (GstPad * pad)
{
  static GstFormat src_formats[] = {
    GST_FORMAT_BYTES,
    GST_FORMAT_DEFAULT,         /* granulepos */
    GST_FORMAT_TIME,
    0
  };
  static GstFormat sink_formats[] = {
    GST_FORMAT_BYTES,
    GST_FORMAT_DEFAULT,         /* bytes */
    0
  };

  return (GST_PAD_IS_SRC (pad) ? src_formats : sink_formats);
}

static const GstEventMask *
gst_ogg_pad_event_masks (GstPad * pad)
{
  static const GstEventMask src_event_masks[] = {
    {GST_EVENT_SEEK, GST_SEEK_METHOD_SET | GST_SEEK_FLAG_FLUSH},
    {0,}
  };

  return src_event_masks;
}

static const GstQueryType *
gst_ogg_pad_query_types (GstPad * pad)
{
  static const GstQueryType query_types[] = {
    GST_QUERY_START,
    GST_QUERY_SEGMENT_END,
    GST_QUERY_POSITION,
    0
  };

  return query_types;
}

static gboolean
gst_ogg_pad_src_convert (GstPad * pad,
    GstFormat src_format, gint64 src_value,
    GstFormat * dest_format, gint64 * dest_value)
{
  gboolean res = FALSE;
  GstOggDemux *ogg;

  ogg = GST_OGG_DEMUX (GST_PAD_PARENT (pad));

  /* fill me, not sure with what... */

  return res;
}

static gboolean
gst_ogg_pad_src_query (GstPad * pad, GstQueryType type,
    GstFormat * format, gint64 * value)
{
  gboolean res = TRUE;

  GstOggDemux *ogg;
  GstOggPad *cur;

  ogg = GST_OGG_DEMUX (GST_PAD_PARENT (pad));

  cur = GST_OGG_PAD (pad);

  switch (type) {
    case GST_QUERY_START:
      *value = cur->first_granule;
      break;
    case GST_QUERY_SEGMENT_END:
      *value = cur->last_granule;
      break;
    case GST_QUERY_POSITION:
      *value = cur->current_granule;
      break;
    default:
      res = FALSE;
      break;
  }

  return res;
}

static void
gst_ogg_pad_reset (GstOggPad * pad)
{
  ogg_stream_reset (&pad->stream);
  /* FIXME: need a discont here */
}


/* submit a packet to the oggpad, this function will run the
 * typefind code for the pad if this is the first packet for this
 * stream 
 */
static GstFlowReturn
gst_ogg_pad_submit_packet (GstOggPad * pad, ogg_packet * packet)
{
  GstBuffer *buf;
  gint64 granule;

  GstOggDemux *ogg = pad->ogg;

  GST_DEBUG_OBJECT (ogg,
      "%p submit packet %d, packetno %lld", pad, pad->serialno, pad->packetno);

  granule = packet->granulepos;
  if (granule != -1) {
    pad->current_granule = granule;
  }

  /* first packet */
  if (pad->packetno == 0) {
    GstCaps *caps = gst_ogg_type_find (packet);

    if (caps == NULL) {
      GST_WARNING_OBJECT (ogg,
          "couldn't find caps for stream with serial %d", pad->serialno);
      caps = gst_caps_new_simple ("application/octet-stream", NULL);
    }

    gst_pad_set_caps (GST_PAD (pad), caps);
    gst_caps_unref (caps);
    gst_element_add_pad (GST_ELEMENT (ogg), GST_PAD (pad));
  }

  pad->packetno++;

  buf =
      gst_pad_alloc_buffer (GST_PAD (pad), GST_BUFFER_OFFSET_NONE,
      packet->bytes, GST_PAD_CAPS (pad));
  if (buf) {
    memcpy (buf->data, packet->packet, packet->bytes);
    GST_BUFFER_OFFSET (buf) = pad->offset;
    GST_BUFFER_OFFSET_END (buf) = packet->granulepos;
    pad->offset = packet->granulepos;

    return gst_pad_push (GST_PAD (pad), buf);
  }
  return GST_FLOW_ERROR;
}

/* submit a page to an oggpad, this function will then submit all
 * the packets in the page.
 */
static GstFlowReturn
gst_ogg_pad_submit_page (GstOggPad * pad, ogg_page * page)
{
  ogg_packet packet;
  int ret;
  gboolean done = FALSE;
  GstFlowReturn result = GST_FLOW_OK;
  GstOggDemux *ogg;

  ogg = GST_OGG_DEMUX (GST_PAD_PARENT (pad));

  if (ogg_stream_pagein (&pad->stream, page) != 0) {
    GST_WARNING_OBJECT (ogg,
        "ogg stream choked on page (serial %d), resetting stream",
        pad->serialno);
    gst_ogg_pad_reset (pad);
    return GST_FLOW_OK;
  }

  while (!done) {
    ret = ogg_stream_packetout (&pad->stream, &packet);
    GST_LOG_OBJECT (ogg, "packetout gave %d", ret);
    switch (ret) {
      case 0:
        done = TRUE;
        break;
      case -1:
        /* out of sync, could call gst_ogg_pad_reset() here but ogg can decode
         * the packet just fine. We should probably send a DISCONT though. */
        break;
      case 1:
        result = gst_ogg_pad_submit_packet (pad, &packet);
        if (result != GST_FLOW_OK) {
          done = TRUE;
        }
        break;
      default:
        GST_WARNING_OBJECT (ogg,
            "invalid return value %d for ogg_stream_packetout, resetting stream",
            ret);
        gst_ogg_pad_reset (pad);
        break;
    }
  }
  return result;
}

/* all information needed for one ogg chain (relevant for chained bitstreams) */
typedef struct
{
  GstOggDemux *ogg;

  gint64 offset;                /* starting offset of chain */
  gint64 end_offset;            /* end offset of chain */
  gint64 bytes;                 /* number of bytes */

  gboolean have_bos;

  GArray *streams;
}
GstOggChain;

static GstOggChain *
gst_ogg_chain_new (GstOggDemux * ogg)
{
  GstOggChain *chain = g_new0 (GstOggChain, 1);

  GST_DEBUG_OBJECT (ogg, "creating new chain %p", chain);
  chain->ogg = ogg;
  chain->offset = -1;
  chain->bytes = -1;
  chain->have_bos = FALSE;
  chain->streams = g_array_new (FALSE, TRUE, sizeof (GstOggPad *));

  return chain;
}

#if 0
static void
gst_ogg_chain_free (GstOggChain * chain)
{
  g_array_free (chain->streams, TRUE);
}
#endif

static GstOggPad *
gst_ogg_chain_new_stream (GstOggChain * chain, glong serialno)
{
  GstOggPad *ret;
  GstTagList *list;
  gchar *name;

  GST_DEBUG_OBJECT (chain->ogg, "creating new stream %ld in chain %p", serialno,
      chain);

  ret = g_object_new (GST_TYPE_OGG_PAD, NULL);
  /* we own this one */
  gst_object_ref (GST_OBJECT (ret));
  gst_object_sink (GST_OBJECT (ret));

  list = gst_tag_list_new ();
  name = g_strdup_printf ("serial_%ld", serialno);

  GST_RPAD_DIRECTION (ret) = GST_PAD_SRC;
  ret->ogg = chain->ogg;
  gst_object_set_name (GST_OBJECT (ret), name);
  g_free (name);

  ret->first_granule = -1;
  ret->last_granule = -1;
  ret->current_granule = -1;

  ret->serialno = serialno;
  if (ogg_stream_init (&ret->stream, serialno) != 0) {
    GST_ERROR ("Could not initialize ogg_stream struct for serial %d.",
        serialno);
    g_object_unref (G_OBJECT (ret));
    return NULL;
  }
  gst_tag_list_add (list, GST_TAG_MERGE_REPLACE, GST_TAG_SERIAL, serialno,
      NULL);
  //gst_element_found_tags (GST_ELEMENT (ogg), list);
  gst_tag_list_free (list);

  GST_LOG ("created new ogg src %p for stream with serial %d", ret, serialno);

  g_array_append_val (chain->streams, ret);

  return ret;
}

static GstOggPad *
gst_ogg_chain_get_stream (GstOggChain * chain, glong serialno)
{
  gint i;

  for (i = 0; i < chain->streams->len; i++) {
    GstOggPad *pad = g_array_index (chain->streams, GstOggPad *, i);

    if (pad->serialno == serialno)
      return pad;
  }
  return NULL;
}

static gboolean
gst_ogg_chain_has_stream (GstOggChain * chain, glong serialno)
{
  return gst_ogg_chain_get_stream (chain, serialno) != NULL;
}

#define CURRENT_CHAIN(ogg) (&g_array_index ((ogg)->chains, GstOggChain, (ogg)->current_chain))

typedef enum
{
  OGG_STATE_NEW_CHAIN,
  OGG_STATE_STREAMING,
} OggState;

struct _GstOggDemux
{
  GstElement element;

  GstPad *sinkpad;

  gint64 length;
  gint64 offset;

  OggState state;

  /* state */
  GArray *chains;               /* list of chains we know */

  GstOggChain *current_chain;
  GstOggChain *building_chain;

  /* ogg stuff */
  ogg_sync_state sync;
};

struct _GstOggDemuxClass
{
  GstElementClass parent_class;
};

/* signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0
      /* FILL ME */
};

static GstStaticPadTemplate ogg_demux_src_template_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate ogg_demux_sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/ogg")
    );

static void gst_ogg_demux_finalize (GObject * object);

//static const GstEventMask *gst_ogg_demux_get_event_masks (GstPad * pad);
//static const GstQueryType *gst_ogg_demux_get_query_types (GstPad * pad);
static GstOggChain *gst_ogg_demux_read_chain (GstOggDemux * ogg);

static gboolean gst_ogg_demux_handle_event (GstPad * pad, GstEvent * event);
static gboolean gst_ogg_demux_loop (GstOggPad * pad);
static GstFlowReturn gst_ogg_demux_chain (GstPad * pad, GstBuffer * buffer);
static gboolean gst_ogg_demux_sink_activate (GstPad * sinkpad,
    GstActivateMode mode);
static GstElementStateReturn gst_ogg_demux_change_state (GstElement * element);

static void gst_ogg_print (GstOggDemux * demux);

GST_BOILERPLATE (GstOggDemux, gst_ogg_demux, GstElement, GST_TYPE_ELEMENT);

static void
gst_ogg_demux_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  static GstElementDetails gst_ogg_demux_details =
      GST_ELEMENT_DETAILS ("ogg demuxer",
      "Codec/Demuxer",
      "demux ogg streams (info about ogg: http://xiph.org)",
      "Wim Taymand <wim@fluendo.com>");

  gst_element_class_set_details (element_class, &gst_ogg_demux_details);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&ogg_demux_sink_template_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&ogg_demux_src_template_factory));
}
static void
gst_ogg_demux_class_init (GstOggDemuxClass * klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gstelement_class->change_state = gst_ogg_demux_change_state;

  gobject_class->finalize = gst_ogg_demux_finalize;
}

static void
gst_ogg_demux_init (GstOggDemux * ogg)
{
  /* create the sink pad */
  ogg->sinkpad =
      gst_pad_new_from_template (gst_static_pad_template_get
      (&ogg_demux_sink_template_factory), "sink");
  gst_pad_set_formats_function (ogg->sinkpad, gst_ogg_pad_formats);
  gst_pad_set_loop_function (ogg->sinkpad,
      (GstPadLoopFunction) gst_ogg_demux_loop);
  gst_pad_set_event_function (ogg->sinkpad, gst_ogg_demux_handle_event);
  gst_pad_set_chain_function (ogg->sinkpad, gst_ogg_demux_chain);
  gst_pad_set_activate_function (ogg->sinkpad, gst_ogg_demux_sink_activate);
  gst_element_add_pad (GST_ELEMENT (ogg), ogg->sinkpad);

  ogg->chains = g_array_new (FALSE, TRUE, sizeof (GstOggChain *));
  ogg->state = OGG_STATE_NEW_CHAIN;
}

static void
gst_ogg_demux_finalize (GObject * object)
{
  GstOggDemux *ogg;

  ogg = GST_OGG_DEMUX (object);

  ogg_sync_clear (&ogg->sync);

  if (G_OBJECT_CLASS (parent_class)->finalize)
    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_ogg_demux_handle_event (GstPad * pad, GstEvent * event)
{
  GstOggDemux *ogg = GST_OGG_DEMUX (GST_PAD_PARENT (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_DISCONTINUOUS:
      GST_DEBUG_OBJECT (ogg, "got a discont event");
      ogg_sync_reset (&ogg->sync);
      gst_event_unref (event);
      break;
    default:
      return gst_pad_event_default (pad, event);
  }
  return TRUE;
}

/* submit the given buffer to the ogg sync.
 *
 * Returns the number of bytes submited.
 */
static gint
gst_ogg_demux_submit_buffer (GstOggDemux * ogg, GstBuffer * buffer)
{
  guint size;
  guint8 *data;
  gchar *oggbuffer;

  size = GST_BUFFER_SIZE (buffer);
  data = GST_BUFFER_DATA (buffer);

  oggbuffer = ogg_sync_buffer (&ogg->sync, size);
  memcpy (oggbuffer, data, size);
  ogg_sync_wrote (&ogg->sync, size);

  return size;
}

/* in radom access mode this code updates the current read position
 * and resets the ogg sync buffer so that the next read will happen
 * from this new location.
 */
static void
gst_ogg_demux_seek (GstOggDemux * ogg, gint64 offset)
{
  GST_LOG_OBJECT (ogg, "seeking to %lld", offset);

  ogg->offset = offset;
  ogg_sync_reset (&ogg->sync);
}

/* read more data from the current offset and submit to
 * the ogg sync layer.
 *
 * Return number of bytes written.
 */
static gint
gst_ogg_demux_get_data (GstOggDemux * ogg)
{
  GstFlowReturn ret;
  GstBuffer *buffer;
  gint size;

  GST_LOG_OBJECT (ogg, "get data %lld", ogg->offset);
  if (ogg->offset == ogg->length)
    return 0;

  ret = gst_pad_pull_range (ogg->sinkpad, ogg->offset, CHUNKSIZE, &buffer);
  if (ret != GST_FLOW_OK)
    return -1;

  size = gst_ogg_demux_submit_buffer (ogg, buffer);

  return size;
}

/* Read the next page from the current offset.
 */
static gint64
gst_ogg_demux_get_next_page (GstOggDemux * ogg, ogg_page * og, gint64 boundary)
{
  gint64 end_offset = 0;

  GST_LOG_OBJECT (ogg, "get next page %lld", boundary);

  if (boundary > 0)
    end_offset = ogg->offset + boundary;

  while (TRUE) {
    glong more;

    if (boundary > 0 && ogg->offset >= end_offset) {
      GST_LOG_OBJECT (ogg, "offset %lld >= end_offset %lld", ogg->offset,
          end_offset);
      return OV_FALSE;
    }

    more = ogg_sync_pageseek (&ogg->sync, og);

    if (more < 0) {
      GST_LOG_OBJECT (ogg, "skipped %ld bytes", more);
      /* skipped n bytes */
      ogg->offset -= more;
    } else if (more == 0) {
      gint ret;

      /* send more paramedics */
      if (boundary == 0)
        return OV_FALSE;

      ret = gst_ogg_demux_get_data (ogg);
      if (ret == 0)
        return OV_EOF;
      if (ret < 0)
        return OV_EREAD;
    } else {
      /* got a page.  Return the offset at the page beginning,
         advance the internal offset past the page end */
      gint64 ret = ogg->offset;

      ogg->offset += more;

      GST_LOG_OBJECT (ogg, "got page at %lld, serial %08lx, end at %lld", ret,
          ogg_page_serialno (og), ogg->offset);

      return ret;
    }
  }
}

/* from the current offset, find the previous page, seeking backwards
 * until we find the page. */
static gint
gst_ogg_demux_get_prev_page (GstOggDemux * ogg, ogg_page * og)
{
  gint64 begin = ogg->offset;
  gint64 end = begin;
  gint64 ret;
  gint64 offset = -1;

  while (offset == -1) {
    begin -= CHUNKSIZE;
    if (begin < 0)
      begin = 0;

    gst_ogg_demux_seek (ogg, begin);

    /* now continue reading until we run out of data, if we find a page
     * start, we save it. It might not be the final page as there could be
     * another page after this one. */
    while (ogg->offset < end) {
      ret = gst_ogg_demux_get_next_page (ogg, og, end - ogg->offset);
      if (ret == OV_EREAD)
        return OV_EREAD;
      if (ret < 0) {
        break;
      } else {
        offset = ret;
      }
    }
  }

  /* we have the offset.  Actually snork and hold the page now */
  gst_ogg_demux_seek (ogg, offset);
  ret = gst_ogg_demux_get_next_page (ogg, og, CHUNKSIZE);
  if (ret < 0)
    /* this shouldn't be possible */
    return OV_EFAULT;

  return offset;
}

/* finds each bitstream link one at a time using a bisection search
 * (has to begin by knowing the offset of the lb's initial page).
 * Recurses for each link so it can alloc the link storage after
 * finding them all, then unroll and fill the cache at the same time 
 */
static gint
gst_ogg_demux_bisect_forward_serialno (GstOggDemux * ogg,
    gint64 begin, gint64 searched, gint64 end, GstOggChain * chain, glong m)
{
  gint64 endsearched = end;
  gint64 next = end;
  ogg_page og;
  gint64 ret;
  GstOggChain *nextchain;

  GST_LOG_OBJECT (ogg,
      "bisect begin: %lld, searched: %lld, end %lld, chain: %p", begin,
      searched, end, chain);

  /* the below guards against garbage seperating the last and
   * first pages of two links. */
  while (searched < endsearched) {
    gint64 bisect;

    if (endsearched - searched < CHUNKSIZE) {
      bisect = searched;
    } else {
      bisect = (searched + endsearched) / 2;
    }

    gst_ogg_demux_seek (ogg, bisect);
    ret = gst_ogg_demux_get_next_page (ogg, &og, -1);
    if (ret == OV_EREAD) {
      GST_LOG_OBJECT (ogg, "OV_READ");
      return OV_EREAD;
    }

    if (ret < 0 || !gst_ogg_chain_has_stream (chain, ogg_page_serialno (&og))) {
      endsearched = bisect;
      if (ret >= 0)
        next = ret;
    } else {
      searched = ret + og.header_len + og.body_len;
    }
  }

  GST_LOG_OBJECT (ogg, "found begin at %lld", next);

  chain->end_offset = searched;
  gst_ogg_demux_seek (ogg, next);
  nextchain = gst_ogg_demux_read_chain (ogg);

  if (searched < end && nextchain != NULL) {
    ret = gst_ogg_demux_bisect_forward_serialno (ogg, next, ogg->offset,
        end, nextchain, m + 1);

    if (ret == OV_EREAD) {
      GST_LOG_OBJECT (ogg, "OV_READ");
      return OV_EREAD;
    }
  }
  g_array_insert_val (ogg->chains, 0, chain);

  return 0;
}

/* read a chain from the ogg file. This code will
 * read all BOS pages and will create and return a GstOggChain 
 * structure with the results. 
 */
static GstOggChain *
gst_ogg_demux_read_chain (GstOggDemux * ogg)
{
  GstOggChain *chain = NULL;
  gint64 offset = ogg->offset;

  GST_LOG_OBJECT (ogg, "reading chain at %lld", offset);

  while (TRUE) {
    ogg_page og;
    GstOggPad *pad;
    glong serial;
    gint ret;

    ret = gst_ogg_demux_get_next_page (ogg, &og, -1);
    if (ret < 0 || !ogg_page_bos (&og))
      break;

    if (chain == NULL) {
      chain = gst_ogg_chain_new (ogg);
      chain->offset = offset;
    }

    serial = ogg_page_serialno (&og);
    pad = gst_ogg_chain_new_stream (chain, serial);
    pad->first_granule = ogg_page_granulepos (&og);
    pad->current_granule = pad->first_granule;
    pad->last_granule = 0;
    chain->have_bos = TRUE;
  }

  return chain;
}

/* find a pad with a given serial number
 */
static GstOggPad *
gst_ogg_demux_find_pad (GstOggDemux * ogg, int serialno)
{
  GstOggPad *pad;
  gint i;

  for (i = 0; i < ogg->chains->len; i++) {
    GstOggChain *chain = g_array_index (ogg->chains, GstOggChain *, i);

    pad = gst_ogg_chain_get_stream (chain, serialno);
    if (pad)
      return pad;
  }
  return NULL;
}

/* find all the chains in the ogg file, this reads the first and
 * last page of the ogg stream, if they match then the ogg file has
 * just one chain, else we do a binary search for all chains.
 */
static gboolean
gst_ogg_demux_find_chains (GstOggDemux * ogg)
{
  ogg_page og;
  GstPad *peer;
  GstFormat format;
  gboolean res;
  gulong serialno;
  GstOggChain *chain;

  /* get peer to figure out length */
  if ((peer = gst_pad_get_peer (ogg->sinkpad)) == NULL)
    goto no_peer;

  /* find length to read last page, we store this for later use. */
  format = GST_FORMAT_BYTES;
  res = gst_pad_query (peer, GST_QUERY_TOTAL, &format, &ogg->length);
  gst_object_unref (GST_OBJECT (peer));
  if (!res)
    goto no_length;

  /* read chain from offset 0, this is the first chain of the
   * ogg file. */
  gst_ogg_demux_seek (ogg, 0);
  chain = gst_ogg_demux_read_chain (ogg);

  /* read page from end offset, we use this page to check if its serial
   * number is contained in the first chain. If this is the case then
   * this ogg is not a chained ogg and we can skip the scanning. */
  gst_ogg_demux_seek (ogg, ogg->length);
  gst_ogg_demux_get_prev_page (ogg, &og);
  serialno = ogg_page_serialno (&og);

  if (!gst_ogg_chain_has_stream (chain, serialno)) {
    /* the last page is not in the first stream, this means we should
     * find all the chains in this chained ogg. */
    gst_ogg_demux_bisect_forward_serialno (ogg, 0, 0, ogg->length, chain, 0);
  } else {
    /* we still call this function here but with an empty range so that
     * we can reuse the setup code in this routine. */
    gst_ogg_demux_bisect_forward_serialno (ogg, 0, ogg->length, ogg->length,
        chain, 0);
  }
  /* now dump our chains and streams */
  gst_ogg_print (ogg);

  return TRUE;

  /*** error cases ***/
no_peer:
  {
    GST_DEBUG ("we don't have a peer");
    return FALSE;
  }
no_length:
  {
    GST_DEBUG ("can't get file length");
    return FALSE;
  }
}

/* streaming mode, receive a buffer, parse it, create pads for
 * the serialno, submit pages and packets to the oggpads
 */
static GstFlowReturn
gst_ogg_demux_chain (GstPad * pad, GstBuffer * buffer)
{
  GstOggDemux *ogg;
  gint ret = -1;
  GstFlowReturn result = GST_FLOW_OK;

  ogg = GST_OGG_DEMUX (GST_OBJECT_PARENT (pad));

  GST_DEBUG ("chain");
  gst_ogg_demux_submit_buffer (ogg, buffer);

  while (ret != 0 && result == GST_FLOW_OK) {
    ogg_page page;

    ret = ogg_sync_pageout (&ogg->sync, &page);
    if (ret == 0)
      /* need more data */
      break;
    if (ret == -1) {
      /* discontinuity in the pages */
    } else {
      GstOggPad *pad;
      guint serialno;

      serialno = ogg_page_serialno (&page);

      GST_LOG_OBJECT (ogg,
          "processing ogg page (serial %d, pageno %ld, granule pos %llu, bos %d)",
          serialno, ogg_page_pageno (&page),
          ogg_page_granulepos (&page), ogg_page_bos (&page));

      if (ogg_page_bos (&page)) {
        /* first page */
        if (ogg->state == OGG_STATE_STREAMING) {
          /* FIXME, remove previous pads since this is a new BOS when
           * we were in streaming mode. */
          ogg->state = OGG_STATE_NEW_CHAIN;
        }
        pad = gst_ogg_demux_find_pad (ogg, serialno);
        if (pad == NULL) {
          if (ogg->building_chain == NULL) {
            ogg->building_chain = gst_ogg_chain_new (ogg);
            ogg->building_chain->offset = 0;
          }
          pad = gst_ogg_chain_new_stream (ogg->building_chain, serialno);
          pad->first_granule = ogg_page_granulepos (&page);
        }
      } else {
        if (ogg->building_chain) {
          g_array_append_val (ogg->chains, ogg->building_chain);
          ogg->building_chain = NULL;
        }
        ogg->state = OGG_STATE_STREAMING;
        pad = gst_ogg_demux_find_pad (ogg, serialno);
      }
      if (pad) {
        result = gst_ogg_pad_submit_page (pad, &page);
      } else {
        GST_LOG_OBJECT (ogg, "cannot find pad for serial %d", serialno);
      }
    }
  }
  gst_buffer_unref (buffer);

  return result;
}

/* random access code 
 *
 * - first find all the chains and streams by scanning the 
 *   file.
 * - then get and chain buffers, just like the streaming
 *   case.
 * - when seeking, we can use the chain info to perform the
 *   seek.
 */
static gboolean
gst_ogg_demux_loop (GstOggPad * pad)
{
  GstOggDemux *ogg;

  ogg = GST_OGG_DEMUX (GST_OBJECT_PARENT (pad));

  gst_ogg_demux_find_chains (ogg);

  ogg->offset = 0;
  while (TRUE) {
    GstFlowReturn ret;
    GstBuffer *buffer;

    GST_LOG_OBJECT (ogg, "pull data %lld", ogg->offset);
    if (ogg->offset == ogg->length)
      return FALSE;

    ret = gst_pad_pull_range (ogg->sinkpad, ogg->offset, CHUNKSIZE, &buffer);
    if (ret != GST_FLOW_OK) {
      GST_LOG_OBJECT (ogg, "got error %d", ret);
      return FALSE;
    }

    ogg->offset += GST_BUFFER_SIZE (buffer);

    ret = gst_ogg_demux_chain (ogg->sinkpad, buffer);
    if (ret != GST_FLOW_OK) {
      GST_LOG_OBJECT (ogg, "got error %d", ret);
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean
gst_ogg_demux_sink_activate (GstPad * sinkpad, GstActivateMode mode)
{
  gboolean result = FALSE;
  GstOggDemux *ogg;

  ogg = GST_OGG_DEMUX (GST_OBJECT_PARENT (sinkpad));

  switch (mode) {
    case GST_ACTIVATE_PUSH:
      break;
    case GST_ACTIVATE_PULL:
      /* if we have a scheduler we can start the task */
      if (GST_ELEMENT_SCHEDULER (ogg)) {
        GST_STREAM_LOCK (sinkpad);
        GST_RPAD_TASK (sinkpad) =
            gst_scheduler_create_task (GST_ELEMENT_SCHEDULER (ogg),
            (GstTaskFunction) gst_ogg_demux_loop, sinkpad);

        gst_task_start (GST_RPAD_TASK (sinkpad));
        GST_STREAM_UNLOCK (sinkpad);
        result = TRUE;
      }
      break;
    case GST_ACTIVATE_NONE:
      /* step 1, unblock clock sync (if any) */

      /* step 2, make sure streaming finishes */
      GST_STREAM_LOCK (sinkpad);

      /* step 3, stop the task */
      if (GST_RPAD_TASK (sinkpad)) {
        gst_task_stop (GST_RPAD_TASK (sinkpad));
        gst_object_unref (GST_OBJECT (GST_RPAD_TASK (sinkpad)));
        GST_RPAD_TASK (sinkpad) = NULL;
      }
      GST_STREAM_UNLOCK (sinkpad);

      result = TRUE;
      break;
  }
  return result;
}

static GstElementStateReturn
gst_ogg_demux_change_state (GstElement * element)
{
  GstOggDemux *ogg;
  GstElementStateReturn result = GST_STATE_FAILURE;

  ogg = GST_OGG_DEMUX (element);

  switch (GST_STATE_TRANSITION (element)) {
    case GST_STATE_NULL_TO_READY:
      ogg_sync_init (&ogg->sync);
      break;
    case GST_STATE_READY_TO_PAUSED:
      ogg_sync_reset (&ogg->sync);
      break;
    case GST_STATE_PAUSED_TO_PLAYING:
      break;
  }

  result = parent_class->change_state (element);

  switch (GST_STATE_TRANSITION (element)) {
    case GST_STATE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_PAUSED_TO_READY:
      break;
    case GST_STATE_READY_TO_NULL:
      ogg_sync_clear (&ogg->sync);
      break;
    default:
      break;
  }
  return result;
}

/*** typefinding **************************************************************/
/* ogg supports its own typefinding because the ogg spec defines that the first
 * packet of an ogg stream must identify the stream. Therefore ogg can use a
 * simplified approach at typefinding.
 */
typedef struct
{
  ogg_packet *packet;
  guint best_probability;
  GstCaps *caps;
}
OggTypeFind;
static guint8 *
ogg_find_peek (gpointer data, gint64 offset, guint size)
{
  OggTypeFind *find = (OggTypeFind *) data;

  if (offset + size <= find->packet->bytes) {
    return ((guint8 *) find->packet->packet) + offset;
  } else {
    return NULL;
  }
}
static void
ogg_find_suggest (gpointer data, guint probability, const GstCaps * caps)
{
  OggTypeFind *find = (OggTypeFind *) data;

  if (probability > find->best_probability) {
    gst_caps_replace (&find->caps, gst_caps_copy (caps));
    find->best_probability = probability;
  }
}
static GstCaps *
gst_ogg_type_find (ogg_packet * packet)
{
  GstTypeFind gst_find;
  OggTypeFind find;
  GList *walk, *type_list = NULL;

  walk = type_list = gst_type_find_factory_get_list ();

  find.packet = packet;
  find.best_probability = 0;
  find.caps = NULL;
  gst_find.data = &find;
  gst_find.peek = ogg_find_peek;
  gst_find.suggest = ogg_find_suggest;
  gst_find.get_length = NULL;

  while (walk) {
    GstTypeFindFactory *factory = GST_TYPE_FIND_FACTORY (walk->data);

    gst_type_find_factory_call_function (factory, &gst_find);
    if (find.best_probability >= GST_TYPE_FIND_MAXIMUM)
      break;
    walk = g_list_next (walk);
  }

  if (find.best_probability > 0)
    return find.caps;

  return NULL;
}

gboolean
gst_ogg_demux_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_ogg_demux_debug, "oggdemux", 0, "ogg demuxer");
  GST_DEBUG_CATEGORY_INIT (gst_ogg_demux_setup_debug, "oggdemux_setup", 0,
      "ogg demuxer setup stage when parsing pipeline");

  return gst_element_register (plugin, "oggdemux", GST_RANK_PRIMARY,
      GST_TYPE_OGG_DEMUX);
}

/* prints all info about the element */
#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_ogg_demux_setup_debug

#ifdef GST_DISABLE_GST_DEBUG

static void
gst_ogg_print (GstOggDemux * ogg)
{
  /* NOP */
}

#else /* !GST_DISABLE_GST_DEBUG */

static void
gst_ogg_print (GstOggDemux * ogg)
{
  guint j, i;

  for (i = 0; i < ogg->chains->len; i++) {
    GstOggChain *chain = g_array_index (ogg->chains, GstOggChain *, i);

    GST_INFO_OBJECT (ogg, "chain %d (%u streams):", i, chain->streams->len);
    GST_INFO_OBJECT (ogg, " offset: %" G_GINT64_FORMAT " - %" G_GINT64_FORMAT,
        chain->offset, chain->end_offset);

    for (j = 0; j < chain->streams->len; j++) {
      GstOggPad *stream = g_array_index (chain->streams, GstOggPad *, j);

      GST_INFO_OBJECT (ogg, "  stream %08lx:", stream->serialno);
    }
  }
}
#endif /* GST_DISABLE_GST_DEBUG */
