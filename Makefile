cflags = -std=c99 -O0 -g -Wall -Wextra -Wpedantic -Wshadow \
		-Werror=implicit-function-declaration -Werror=vla \
		$(CFLAGS)
ldflags = $(LDFLAGS)

symdir: symdir.c
	$(strip $(CC) $(cflags) -o $@ $^ $(ldflags))
