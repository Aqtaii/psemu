# Cozulemeyen TUM NID'leri genis bir aday isim kumesiyle toplu cozer ve
# nids.h'ye eklenmeye hazir satirlar uretir.
# Kullanim: python nid_bulk.py <eboot.bin>
import base64
import hashlib
import itertools
import re
import struct
import sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(n: str) -> str:
    h = hashlib.sha1(n.encode() + SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().replace("/", "-")[:11]


# ---- oyunun cozulemeyen NID'lerini cikar ----------------------------------
known = {}
for line in open("include/nids.h", encoding="utf-8", errors="replace"):
    m = re.search(r'\{"([A-Za-z0-9+\-]{11})#[^"]*",\s*"([^"]+)"\}', line)
    if m:
        known.setdefault(m.group(1), m.group(2))

data = open(sys.argv[1], "rb").read()
elf_off, self_map = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if fl & 0x800 and es == ds:
            self_map[fl >> 20] = fo

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]
loads, dynv = [], None
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    real = self_map.get(i, elf_off + po)
    if pt == 1:
        loads.append((pv, pf, real))
    elif pt == 2:
        dynv = pv


def v2f(v):
    for pv, pf, real in loads:
        if pv <= v < pv + pf:
            return real + (v - pv)


doff = v2f(dynv)
tags = []
for k in range(0x2000 // 16):
    t, v = struct.unpack_from("<QQ", data, doff + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def g(*n):
    for t, v in tags:
        if t in n:
            return v


jf, pltsz = v2f(g(0x17, 0x61000029)), g(0x2, 0x6100002D)
sf, tf = v2f(g(6, 0x61000039)), v2f(g(5, 0x61000035))

unresolved = {}
for k in range(pltsz // 24):
    ro, ri, _ = struct.unpack_from("<QQq", data, jf + k * 24)
    st = struct.unpack_from("<I", data, sf + (ri >> 32) * 24)[0]
    end = data.index(b"\0", tf + st)
    raw = data[tf + st:end].decode("utf-8", "replace")
    pref = raw.split("#")[0]
    if pref not in known:
        unresolved[pref] = raw

print(f"cozulemeyen: {len(unresolved)}")

# ---- aday isimler ---------------------------------------------------------
cands = set()

LIBC = """
memcpy memmove memset memcmp memchr memrchr memccpy bcopy bzero bcmp
strlen strnlen strcpy strncpy stpcpy stpncpy strcat strncat strcmp strncmp
strcasecmp strncasecmp strcoll strxfrm strchr strrchr strchrnul strspn strcspn
strpbrk strstr strcasestr strtok strtok_r strsep strdup strndup strerror
strerror_r strsignal strlcpy strlcat index rindex ffs ffsl ffsll
malloc calloc realloc reallocarray free aligned_alloc posix_memalign memalign
valloc pvalloc malloc_usable_size mallopt malloc_stats
abort exit _Exit atexit __cxa_atexit __cxa_finalize at_quick_exit quick_exit
system getenv setenv putenv unsetenv clearenv
abs labs llabs imaxabs div ldiv lldiv imaxdiv
rand srand rand_r random srandom initstate setstate arc4random
atoi atol atoll atof strtol strtoll strtoul strtoull strtof strtod strtold
strtoimax strtoumax qsort qsort_r bsearch heapsort mergesort
printf fprintf sprintf snprintf asprintf vasprintf dprintf vdprintf
vprintf vfprintf vsprintf vsnprintf
scanf fscanf sscanf vscanf vfscanf vsscanf
wprintf fwprintf swprintf vswprintf vfwprintf
fopen fdopen freopen fclose fcloseall fflush fread fwrite fseek fseeko ftell
ftello rewind fgetpos fsetpos setvbuf setbuf setbuffer setlinebuf
fgetc getc getchar fgets gets_s ungetc fputc putc putchar fputs puts
feof ferror clearerr fileno perror
remove rename tmpfile tmpnam mkstemp mkdtemp
open openat close read write pread pwrite lseek readv writev
stat fstat lstat fstatat access faccessat chmod fchmod chown fchown
mkdir mkdirat rmdir unlink unlinkat rename renameat link symlink readlink
opendir readdir closedir rewinddir telldir seekdir scandir
getcwd chdir fchdir realpath basename dirname
dup dup2 pipe fcntl ioctl select poll
isalnum isalpha isascii isblank iscntrl isdigit isgraph islower isprint
ispunct isspace isupper isxdigit tolower toupper toascii
iswalnum iswalpha iswblank iswcntrl iswdigit iswgraph iswlower iswprint
iswpunct iswspace iswupper iswxdigit towlower towupper towctrans wctrans
wctype iswctype
wcslen wcsnlen wcscpy wcsncpy wcscat wcsncat wcscmp wcsncmp wcscasecmp
wcscoll wcsxfrm wcschr wcsrchr wcsstr wcstok wcsdup wcsspn wcscspn wcspbrk
wmemcpy wmemmove wmemset wmemcmp wmemchr
mbrtowc wcrtomb mbsrtowcs wcsrtombs mbstowcs wcstombs mbtowc wctomb mblen
mbrlen mbsinit btowc wctob mbrtoc16 c16rtomb mbrtoc32 c32rtomb
setlocale localeconv nl_langinfo duplocale newlocale freelocale uselocale
time clock difftime mktime timegm gmtime gmtime_r localtime localtime_r
asctime asctime_r ctime ctime_r strftime strptime wcsftime
nanosleep clock_gettime clock_settime clock_getres gettimeofday settimeofday
sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh
exp exp2 expm1 log log2 log10 log1p pow sqrt cbrt hypot
ceil floor trunc round lround llround rint nearbyint fabs fmod remainder
frexp ldexp modf scalbn ilogb copysign nan nextafter fdim fmax fmin fma
sinf cosf tanf sqrtf powf expf logf fabsf floorf ceilf fmodf
isnan isinf isfinite signbit fpclassify
setjmp longjmp sigsetjmp siglongjmp signal raise sigaction sigprocmask
sigemptyset sigfillset sigaddset sigdelset sigismember kill
getpid getppid getuid geteuid getgid getegid sysconf getpagesize
mmap munmap mprotect msync madvise mlock munlock
sched_yield usleep sleep alarm
__errno __error __errno_location __assert __assert_fail __assert_rtn
__stack_chk_fail __stack_chk_guard __chk_fail
__memcpy_chk __memmove_chk __memset_chk __strcpy_chk __strcat_chk
__snprintf_chk __sprintf_chk __vsnprintf_chk
_Getctype _Getpctype _Getpwctype _Getwctype _Getptype _Getcoll _Getwcoll
_Gettnames _Getmbcurmax _Getdateorder _Getcvt _Getlconv _Locksyslock
_Unlocksyslock _Lockit _Mbrtowc _Wcrtomb _Tolower _Toupper _Strcoll _Wcscoll
_Strxfrm _Wcsxfrm _Atomic_load _Atomic_store _Fltrounds _Dtest _Dnorm _Dscale
_LDtest _FDtest _Exp _Log _Sin _Cos _Sinh _Cosh _Tan _Atan _Asin _Pow _Sqrt
__cxa_guard_acquire __cxa_guard_release __cxa_guard_abort
__cxa_begin_catch __cxa_end_catch __cxa_rethrow __cxa_throw
__cxa_allocate_exception __cxa_free_exception __cxa_pure_virtual
__cxa_deleted_virtual __cxa_bad_cast __cxa_bad_typeid __cxa_call_unexpected
__cxa_current_exception_type __cxa_uncaught_exception __cxa_uncaught_exceptions
__cxa_get_globals __cxa_get_globals_fast __cxa_demangle __cxa_thread_atexit
__dynamic_cast __gxx_personality_v0 __tls_get_addr
_Unwind_Resume _Unwind_RaiseException _Unwind_DeleteException _Unwind_Backtrace
_Unwind_GetIP _Unwind_SetIP _Unwind_GetCFA _Unwind_GetLanguageSpecificData
_Unwind_GetRegionStart _Unwind_SetGR _Unwind_GetGR _Unwind_ForcedUnwind
_Znwm _Znam _ZdlPv _ZdaPv _ZdlPvm _ZdaPvm
uncaught_exception uncaught_exceptions terminate unexpected
"""
for n in LIBC.split():
    cands.add(n)

# operator new/delete varyantlari
for base in ("_Znwm", "_Znam", "_ZdlPv", "_ZdaPv"):
    cands.add(base)
    for suf in ("m", "RKSt9nothrow_t", "St11align_val_t",
                "St11align_val_tRKSt9nothrow_t", "mSt11align_val_t"):
        cands.add(base + suf)

# sceLibc* aile
for f in ("HeapGetTraceInfo", "MspaceCreate", "MspaceDestroy", "MspaceMalloc",
          "MspaceFree", "MspaceCalloc", "MspaceRealloc", "MspaceMemalign",
          "MspaceReallocalign", "MspaceMallocUsableSize", "MspaceMallocStats",
          "MspaceMallocStatsFast", "MspaceIsHeapEmpty", "MspacePosixMemalign",
          "SetHeapAllocationCallback", "Once", "Init", "Exit",
          "HeapGetAddressRanges", "HeapSetTraceInfo", "HeapUnsetTraceInfo"):
    cands.add("sceLibc" + f)

# ---- indirilen topluluk veritabanlari (varsa) ------------------------------
# tools/nid_db/ gitignore'da: lisansi belirsiz, repoya girmiyor. Cozulen isimler
# nids.h'ye BIZIM satirlarimiz olarak yaziliyor, veritabanini tasimaya gerek yok.
#   known_names.txt : satir basina bir isim -> NID'i biz hesapliyoruz
#   aerolib.csv     : "NID isim" -> hazir eslesme, hash'e hic gerek yok
import os

table = {}
db_direct = 0
try:
    with open("tools/nid_db/aerolib.csv", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 11:
                table.setdefault(parts[0], parts[1])
                db_direct += 1
except OSError:
    pass

db_names = 0
try:
    with open("tools/nid_db/known_names.txt", encoding="utf-8", errors="replace") as f:
        for line in f:
            n = line.strip()
            if n:
                cands.add(n)
                db_names += 1
except OSError:
    pass

print(f"veritabani: aerolib {db_direct} hazir eslesme, known_names {db_names} isim")

hits, misses = [], []
for c in cands:
    table.setdefault(nid(c), c)
for pref, raw in sorted(unresolved.items()):
    name = table.get(pref)
    (hits if name else misses).append((pref, raw, name))

print(f"aday isim: {len(cands)}")
print(f"COZULDU: {len(hits)}   kalan: {len(misses)}\n")
if hits:
    print("--- nids.h'ye eklenecek satirlar ---")
    for pref, raw, name in hits:
        suffix = raw[len(pref):]
        print(f'    {{"{pref}{suffix}", "{name}"}},')
