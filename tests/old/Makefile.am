SUBDIRS=# spider #autoplug seeking
DIST_SUBDIRS=autoplug spider

GST_PLUGIN_PATH=$(shell cd $(top_builddir) && pwd)

$(TESTS):
	@echo -e '\nrunning gst-register-@GST_MAJORMINOR@...\n'
	$(GST_TOOLS_DIR)/gst-register-@GST_MAJORMINOR@ --gst-plugin-path=$(GST_PLUGIN_PATH)

TESTS=$(GST_TOOLS_DIR)/gst-compprep-@GST_MAJORMINOR@

.PHONY: $(TESTS)
