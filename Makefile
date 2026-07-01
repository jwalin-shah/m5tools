TOOLS = m5fand m5logd m5mon

all:
	$(foreach t,$(TOOLS),$(MAKE) -C $(t);)

install:
	$(foreach t,$(TOOLS),$(MAKE) -C $(t) install;)

clean:
	$(foreach t,$(TOOLS),$(MAKE) -C $(t) clean;)

.PHONY: all install clean $(TOOLS)
