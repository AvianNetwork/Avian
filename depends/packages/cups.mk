package=cups

$(package)_version=2.4.7
$(package)_download_path=https://github.com/OpenPrinting/cups/releases/download/v$($(package)_version)
$(package)_file_name=cups-$($(package)_version)-source.tar.gz
$(package)_sha256_hash=dd54228dd903526428ce7e37961afaed230ad310788141da75cebaa08362cf6c

$(package)_dependencies=

define $(package)_set_vars
  $(package)_config_opts=--disable-shared --enable-static
  $(package)_config_opts+=--disable-dbus --disable-libpaper --disable-pam
  $(package)_config_opts+=--disable-gssapi --disable-avahi --without-systemd
  $(package)_config_opts+=--with-components=libcups --with-tls=no
endef

define $(package)_preprocess_cmds
  sed 's|#ifdef HAVE_OPENSSL|#if 0|g' cups/hash.c > cups/hash.c.tmp && mv cups/hash.c.tmp cups/hash.c && \
  sed 's|#else // HAVE_GNUTLS|#elif 0|g' cups/hash.c > cups/hash.c.tmp && mv cups/hash.c.tmp cups/hash.c
endef

define $(package)_config_cmds
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE) -C cups libs
endef

define $(package)_stage_cmds
  $(MAKE) -C cups DESTDIR=$($(package)_staging_dir) install-headers install-libs && \
  $($(package)_ranlib) $($(package)_staging_dir)$(host_prefix)/lib/libcups.a
endef
