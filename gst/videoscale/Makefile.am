plugindir = $(libdir)/gstreamer-@GST_MAJORMINOR@

plugin_LTLIBRARIES = libgstvideoscale.la

ARCHSRCS =

libgstvideoscale_la_SOURCES = \
			gstvideoscale.c \
			videoscale.c \
			$(ARCHSRCS)
libgstvideoscale_la_CFLAGS = -O2 $(FOMIT_FRAME_POINTER) -funroll-all-loops -finline-functions -ffast-math $(GST_CFLAGS)
libgstvideoscale_la_LIBADD =
libgstvideoscale_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

noinst_HEADERS = gstvideoscale.h videoscale.h videoscale_x86.h
