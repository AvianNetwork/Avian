#!/bin/sh
# Copyright (c) 2017-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Install libdb4.8 (Berkeley DB).

export LC_ALL=C
set -e

if [ -z "${1}" ]; then
  echo "Usage: $0 <base-dir> [<extra-bdb-configure-flag> ...]"
  echo
  echo "Must specify a single argument: the directory in which db4 will be built."
  echo "This is probably \`pwd\` if you're at the root of the Avian repository."
  exit 1
fi

expand_path() {
  cd "${1}" && pwd -P
}

BDB_PREFIX="$(expand_path ${1})/db4"; shift;
BDB_VERSION='db-4.8.30.NC'
BDB_HASH='e0491a07cdb21fb9aa82773bbbedaeb7639cbd0e7f96147ab46141e0045db72a'

# Try multiple mirrors for Berkeley DB (verified working mirrors)
BDB_URL_1="https://archive.debian.org/debian/pool/main/d/db4.8/db4.8_4.8.30.orig.tar.gz"
BDB_URL_2="https://download.oracle.com/berkeley-db/db-4.8.30.tar.gz"
BDB_URL_3="https://distfiles.macports.org/db48/db-4.8.30.tar.gz"
BDB_URL_4="https://ftp.osuosl.org/pub/blfs/conglomeration/db/db-4.8.30.tar.gz"
BDB_URL_5="https://fossies.org/linux/misc/db-4.8.30.tar.gz"

check_exists() {
  command -v "$1" >/dev/null
}

sha256_check() {
  # Args: <sha256_hash> <filename>
  #
  if check_exists sha256sum; then
    echo "${1}  ${2}" | sha256sum -c
  elif check_exists sha256; then
    if [ "$(uname)" = "FreeBSD" ]; then
      sha256 -c "${1}" "${2}"
    else
      echo "${1}  ${2}" | sha256 -c
    fi
  else
    echo "${1}  ${2}" | shasum -a 256 -c
  fi
}

http_get() {
  # Args: <filename> <sha256_hash>
  #
  # It's acceptable that we don't require SSL here because we manually verify
  # content hashes below.
  #
  if [ -f "${1}" ]; then
    echo "File ${1} already exists; not downloading again"
    return 0
  fi

  success=0
  for i in 1 2 3 4 5; do
    eval "url=\$BDB_URL_$i"
    echo "Trying to download from: $url"
    if check_exists curl; then
      if curl --insecure --retry 3 --connect-timeout 30 "${url}" -o "${1}"; then
        if sha256_check "${2}" "${1}"; then
          echo "Successfully downloaded and verified ${1}"
          success=1
          break
        else
          echo "Checksum verification failed for ${url}"
          rm -f "${1}"
        fi
      else
        echo "Download failed from ${url}"
      fi
    else
      if wget --no-check-certificate --timeout=30 "${url}" -O "${1}"; then
        if sha256_check "${2}" "${1}"; then
          echo "Successfully downloaded and verified ${1}"
          success=1
          break
        else
          echo "Checksum verification failed for ${url}"
          rm -f "${1}"
        fi
      else
        echo "Download failed from ${url}"
      fi
    fi
  done

  if [ $success -eq 0 ]; then
    echo "ERROR: Failed to download ${1} from any mirror"
    exit 1
  fi
}

mkdir -p "${BDB_PREFIX}"
http_get "${BDB_VERSION}.tar.gz" "${BDB_HASH}"

# Extract the tar file
echo "Extracting ${BDB_VERSION}.tar.gz..."
if ! tar -xzvf ${BDB_VERSION}.tar.gz -C "$BDB_PREFIX"; then
    echo "Error: Failed to extract ${BDB_VERSION}.tar.gz"
    exit 1
fi

# Check if the extracted directory exists and cd into it
if [ -d "${BDB_PREFIX}/${BDB_VERSION}" ]; then
    echo "Found directory: ${BDB_PREFIX}/${BDB_VERSION}"
    cd "${BDB_PREFIX}/${BDB_VERSION}/"
elif [ -d "${BDB_PREFIX}/db-4.8.30" ]; then
    echo "Found directory: ${BDB_PREFIX}/db-4.8.30"
    cd "${BDB_PREFIX}/db-4.8.30/"
else
    echo "Error: Could not find extracted Berkeley DB directory"
    echo "Expected: ${BDB_PREFIX}/${BDB_VERSION} or ${BDB_PREFIX}/db-4.8.30"
    echo "Contents of ${BDB_PREFIX}:"
    ls -la "${BDB_PREFIX}"
    echo "Contents of current directory:"
    ls -la
    exit 1
fi

# Apply a patch necessary when building with clang and c++11 (see https://community.oracle.com/thread/3952592)
patch --ignore-whitespace -p1 << 'EOF'
commit 3311d68f11d1697565401eee6efc85c34f022ea7
Author: fanquake <fanquake@gmail.com>
Date:   Mon Aug 17 20:03:56 2020 +0800

    Fix C++11 compatibility

diff --git a/dbinc/atomic.h b/dbinc/atomic.h
index 0034dcc..7c11d4a 100644
--- a/dbinc/atomic.h
+++ b/dbinc/atomic.h
@@ -70,7 +70,7 @@ typedef struct {
  * These have no memory barriers; the caller must include them when necessary.
  */
 #define	atomic_read(p)		((p)->value)
-#define	atomic_init(p, val)	((p)->value = (val))
+#define	atomic_init_db(p, val)	((p)->value = (val))

 #ifdef HAVE_ATOMIC_SUPPORT

@@ -144,7 +144,7 @@ typedef LONG volatile *interlocked_val;
 #define	atomic_inc(env, p)	__atomic_inc(p)
 #define	atomic_dec(env, p)	__atomic_dec(p)
 #define	atomic_compare_exchange(env, p, o, n)	\
-	__atomic_compare_exchange((p), (o), (n))
+	__atomic_compare_exchange_db((p), (o), (n))
 static inline int __atomic_inc(db_atomic_t *p)
 {
 	int	temp;
@@ -176,7 +176,7 @@ static inline int __atomic_dec(db_atomic_t *p)
  * http://gcc.gnu.org/onlinedocs/gcc-4.1.0/gcc/Atomic-Builtins.html
  * which configure could be changed to use.
  */
-static inline int __atomic_compare_exchange(
+static inline int __atomic_compare_exchange_db(
 	db_atomic_t *p, atomic_value_t oldval, atomic_value_t newval)
 {
 	atomic_value_t was;
@@ -206,7 +206,7 @@ static inline int __atomic_compare_exchange(
 #define	atomic_dec(env, p)	(--(p)->value)
 #define	atomic_compare_exchange(env, p, oldval, newval)		\
 	(DB_ASSERT(env, atomic_read(p) == (oldval)),		\
-	atomic_init(p, (newval)), 1)
+	atomic_init_db(p, (newval)), 1)
 #else
 #define atomic_inc(env, p)	__atomic_inc(env, p)
 #define atomic_dec(env, p)	__atomic_dec(env, p)
diff --git a/mp/mp_fget.c b/mp/mp_fget.c
index 5fdee5a..0b75f57 100644
--- a/mp/mp_fget.c
+++ b/mp/mp_fget.c
@@ -617,7 +617,7 @@ alloc:		/* Allocate a new buffer header and data space. */

 		/* Initialize enough so we can call __memp_bhfree. */
 		alloc_bhp->flags = 0;
-		atomic_init(&alloc_bhp->ref, 1);
+		atomic_init_db(&alloc_bhp->ref, 1);
 #ifdef DIAGNOSTIC
 		if ((uintptr_t)alloc_bhp->buf & (sizeof(size_t) - 1)) {
 			__db_errx(env,
@@ -911,7 +911,7 @@ alloc:		/* Allocate a new buffer header and data space. */
 			MVCC_MPROTECT(bhp->buf, mfp->stat.st_pagesize,
 			    PROT_READ);

-		atomic_init(&alloc_bhp->ref, 1);
+		atomic_init_db(&alloc_bhp->ref, 1);
 		MUTEX_LOCK(env, alloc_bhp->mtx_buf);
 		alloc_bhp->priority = bhp->priority;
 		alloc_bhp->pgno = bhp->pgno;
diff --git a/mp/mp_mvcc.c b/mp/mp_mvcc.c
index 34467d2..f05aa0c 100644
--- a/mp/mp_mvcc.c
+++ b/mp/mp_mvcc.c
@@ -276,7 +276,7 @@ __memp_bh_freeze(dbmp, infop, hp, bhp, need_frozenp)
 #else
 	memcpy(frozen_bhp, bhp, SSZA(BH, buf));
 #endif
-	atomic_init(&frozen_bhp->ref, 0);
+	atomic_init_db(&frozen_bhp->ref, 0);
 	if (mutex != MUTEX_INVALID)
 		frozen_bhp->mtx_buf = mutex;
 	else if ((ret = __mutex_alloc(env, MTX_MPOOL_BH,
@@ -428,7 +428,7 @@ __memp_bh_thaw(dbmp, infop, hp, frozen_bhp, alloc_bhp)
 #endif
 		alloc_bhp->mtx_buf = mutex;
 		MUTEX_LOCK(env, alloc_bhp->mtx_buf);
-		atomic_init(&alloc_bhp->ref, 1);
+		atomic_init_db(&alloc_bhp->ref, 1);
 		F_CLR(alloc_bhp, BH_FROZEN);
 	}

diff --git a/mp/mp_region.c b/mp/mp_region.c
index e6cece9..ddbe906 100644
--- a/mp/mp_region.c
+++ b/mp/mp_region.c
@@ -224,7 +224,7 @@ __memp_init(env, dbmp, reginfo_off, htab_buckets, max_nreg)
 			     MTX_MPOOL_FILE_BUCKET, 0, &htab[i].mtx_hash)) != 0)
 				return (ret);
 			SH_TAILQ_INIT(&htab[i].hash_bucket);
-			atomic_init(&htab[i].hash_page_dirty, 0);
+			atomic_init_db(&htab[i].hash_page_dirty, 0);
 		}

 		/*
@@ -269,7 +269,7 @@ __memp_init(env, dbmp, reginfo_off, htab_buckets, max_nreg)
 		hp->mtx_hash = (mtx_base == MUTEX_INVALID) ? MUTEX_INVALID :
 		    mtx_base + i;
 		SH_TAILQ_INIT(&hp->hash_bucket);
-		atomic_init(&hp->hash_page_dirty, 0);
+		atomic_init_db(&hp->hash_page_dirty, 0);
 #ifdef HAVE_STATISTICS
 		hp->hash_io_wait = 0;
 		hp->hash_frozen = hp->hash_thawed = hp->hash_frozen_freed = 0;
diff --git a/mutex/mut_method.c b/mutex/mut_method.c
index 2588763..5c6d516 100644
--- a/mutex/mut_method.c
+++ b/mutex/mut_method.c
@@ -426,7 +426,7 @@ atomic_compare_exchange(env, v, oldval, newval)
 	MUTEX_LOCK(env, mtx);
 	ret = atomic_read(v) == oldval;
 	if (ret)
-		atomic_init(v, newval);
+		atomic_init_db(v, newval);
 	MUTEX_UNLOCK(env, mtx);

 	return (ret);
diff --git a/mutex/mut_tas.c b/mutex/mut_tas.c
index f3922e0..e40fcdf 100644
--- a/mutex/mut_tas.c
+++ b/mutex/mut_tas.c
@@ -46,7 +46,7 @@ __db_tas_mutex_init(env, mutex, flags)

 #ifdef HAVE_SHARED_LATCHES
 	if (F_ISSET(mutexp, DB_MUTEX_SHARED))
-		atomic_init(&mutexp->sharecount, 0);
+		atomic_init_db(&mutexp->sharecount, 0);
 	else
 #endif
 	if (MUTEX_INIT(&mutexp->tas)) {
@@ -486,7 +486,7 @@ __db_tas_mutex_unlock(env, mutex)
 			F_CLR(mutexp, DB_MUTEX_LOCKED);
 			/* Flush flag update before zeroing count */
 			MEMBAR_EXIT();
-			atomic_init(&mutexp->sharecount, 0);
+			atomic_init_db(&mutexp->sharecount, 0);
 		} else {
 			DB_ASSERT(env, sharecount > 0);
 			MEMBAR_EXIT();
EOF

# The packaged config.guess and config.sub are ancient (2009) and can cause build issues.
# Replace them with modern versions.
# See https://github.com/bitcoin/bitcoin/issues/16064

# Updated URLs for config.guess and config.sub
CONFIG_GUESS_URL_1="https://ftp.gnu.org/gnu/config/config.guess"
CONFIG_GUESS_URL_2="https://raw.githubusercontent.com/gcc-mirror/gcc/master/config.guess"
CONFIG_GUESS_URL_3="https://github.com/gcc-mirror/gcc/raw/master/config.guess"

CONFIG_SUB_URL_1="https://ftp.gnu.org/gnu/config/config.sub"
CONFIG_SUB_URL_2="https://raw.githubusercontent.com/gcc-mirror/gcc/master/config.sub"
CONFIG_SUB_URL_3="https://github.com/gcc-mirror/gcc/raw/master/config.sub"

rm -f "dist/config.guess"
rm -f "dist/config.sub"

# Download config.guess
echo "Downloading config.guess..."
for i in 1 2 3; do
  eval "url=\$CONFIG_GUESS_URL_$i"
  echo "Trying: $url"
  if check_exists curl; then
    if curl --insecure --retry 3 --connect-timeout 30 "${url}" -o dist/config.guess; then
      # Check if file is valid (not an HTML error page)
      if [ -f "dist/config.guess" ] && [ $(wc -c < "dist/config.guess") -gt 1000 ] && ! grep -q "<html>" "dist/config.guess"; then
        echo "Successfully downloaded config.guess"
        break
      else
        echo "Downloaded file appears to be invalid, trying next URL..."
        rm -f dist/config.guess
      fi
    fi
  else
    if wget --no-check-certificate --timeout=30 "${url}" -O dist/config.guess; then
      # Check if file is valid (not an HTML error page)
      if [ -f "dist/config.guess" ] && [ $(wc -c < "dist/config.guess") -gt 1000 ] && ! grep -q "<html>" "dist/config.guess"; then
        echo "Successfully downloaded config.guess"
        break
      else
        echo "Downloaded file appears to be invalid, trying next URL..."
        rm -f dist/config.guess
      fi
    fi
  fi
done

# Download config.sub
echo "Downloading config.sub..."
for i in 1 2 3; do
  eval "url=\$CONFIG_SUB_URL_$i"
  echo "Trying: $url"
  if check_exists curl; then
    if curl --insecure --retry 3 --connect-timeout 30 "${url}" -o dist/config.sub; then
      # Check if file is valid (not an HTML error page)
      if [ -f "dist/config.sub" ] && [ $(wc -c < "dist/config.sub") -gt 1000 ] && ! grep -q "<html>" "dist/config.sub"; then
        echo "Successfully downloaded config.sub"
        break
      else
        echo "Downloaded file appears to be invalid, trying next URL..."
        rm -f dist/config.sub
      fi
    fi
  else
    if wget --no-check-certificate --timeout=30 "${url}" -O dist/config.sub; then
      # Check if file is valid (not an HTML error page)
      if [ -f "dist/config.sub" ] && [ $(wc -c < "dist/config.sub") -gt 1000 ] && ! grep -q "<html>" "dist/config.sub"; then
        echo "Successfully downloaded config.sub"
        break
      else
        echo "Downloaded file appears to be invalid, trying next URL..."
        rm -f dist/config.sub
      fi
    fi
  fi
done

# Make sure we have the files and they're valid
if [ ! -f "dist/config.guess" ] || [ ! -f "dist/config.sub" ]; then
  echo "ERROR: Failed to download config.guess or config.sub"
  exit 1
fi

# Final validation
if [ $(wc -c < "dist/config.guess") -lt 1000 ] || grep -q "<html>" "dist/config.guess"; then
  echo "ERROR: config.guess appears to be invalid"
  exit 1
fi

if [ $(wc -c < "dist/config.sub") -lt 1000 ] || grep -q "<html>" "dist/config.sub"; then
  echo "ERROR: config.sub appears to be invalid"
  exit 1
fi

cd build_unix/

# Determine the correct path to the configure script
if [ -f "${BDB_PREFIX}/${BDB_VERSION}/dist/configure" ]; then
    CONFIGURE_PATH="${BDB_PREFIX}/${BDB_VERSION}/dist/configure"
elif [ -f "${BDB_PREFIX}/db-4.8.30/dist/configure" ]; then
    CONFIGURE_PATH="${BDB_PREFIX}/db-4.8.30/dist/configure"
else
    echo "Error: Could not find configure script"
    echo "Looking for: ${BDB_PREFIX}/${BDB_VERSION}/dist/configure or ${BDB_PREFIX}/db-4.8.30/dist/configure"
    exit 1
fi

echo "Configuring Berkeley DB..."
if ! "${CONFIGURE_PATH}" \
  --enable-cxx --disable-shared --disable-replication --with-pic --prefix="${BDB_PREFIX}" \
  "${@}"; then
    echo "Error: Configure failed"
    exit 1
fi

echo "Building Berkeley DB..."
if ! make; then
    echo "Error: Make failed"
    exit 1
fi

echo "Installing Berkeley DB..."
if ! make install; then
    echo "Error: Make install failed"
    exit 1
fi

echo
echo "db4 build complete."
echo
# shellcheck disable=SC2016
echo 'When compiling Avian, run `./configure` in the following way:'
echo
echo "  export BDB_PREFIX='${BDB_PREFIX}'"
# shellcheck disable=SC2016
echo '  ./configure BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" BDB_CFLAGS="-I${BDB_PREFIX}/include" ...'
