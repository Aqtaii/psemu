# Astro Bot'un cozulemeyen libc NID'lerini cozmeyi dener.
#
# Modul tablosundan biliyoruz: sonek "#s#s" -> modul id 44 -> libc.
# Yani arama alani C standart kutuphanesi + Sony libc uzantilari.
#
# Once ALGORITMAYI nids.h'deki BILINEN ciftlerle dogruluyoruz: iki farkli surum
# dolasimda (tuzlu / tuzsuz), hangisinin gercekten uydugunu olcerek seciyoruz.
import base64
import hashlib
import re
import sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid_salted(n: str) -> str:
    h = hashlib.sha1(n.encode() + SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().replace("/", "-")[:11]


def nid_plain(n: str) -> str:
    h = hashlib.sha1(n.encode()).digest()[:8]
    return base64.b64encode(h).decode().replace("/", "-")[:11]


# --- 1) nids.h'den bilinen ciftleri oku ve iki surumu de sina ---------------
pairs = []
for line in open("include/nids.h", encoding="utf-8", errors="replace"):
    m = re.search(r'\{"([A-Za-z0-9+\-]{11})#[^"]*",\s*"([^"]+)"\}', line)
    if m:
        pairs.append((m.group(1), m.group(2)))

seen = set()
uniq = []
for k, v in pairs:
    if (k, v) not in seen:
        seen.add((k, v))
        uniq.append((k, v))

score = {"salted": 0, "plain": 0}
for k, v in uniq:
    if nid_salted(v) == k:
        score["salted"] += 1
    if nid_plain(v) == k:
        score["plain"] += 1
print(f"nids.h'den {len(uniq)} benzersiz cift okundu")
print(f"  tuzlu surum  : {score['salted']} eslesme")
print(f"  tuzsuz surum : {score['plain']} eslesme")

nid = nid_salted if score["salted"] >= score["plain"] else nid_plain
print(f"-> kullanilacak: {'tuzlu' if nid is nid_salted else 'tuzsuz'}\n")

# --- 2) Aday libc isimleri --------------------------------------------------
C = """
memcpy memmove memset memcmp memchr strlen strcpy strncpy strcat strncat strcmp
strncmp strcoll strxfrm strchr strrchr strspn strcspn strpbrk strstr strtok
strerror strdup strndup strnlen strlcpy strlcat strcasecmp strncasecmp
malloc calloc realloc free aligned_alloc posix_memalign memalign valloc
abort exit _Exit atexit at_quick_exit quick_exit system getenv setenv unsetenv
abs labs llabs div ldiv lldiv rand srand rand_r
atoi atol atoll atof strtol strtoll strtoul strtoull strtof strtod strtold
qsort bsearch
printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf
scanf fscanf sscanf vscanf vfscanf vsscanf
fopen freopen fclose fflush fread fwrite fseek ftell fseeko ftello rewind
fgetc fgets fputc fputs getc putc getchar putchar puts ungetc setvbuf setbuf
feof ferror clearerr perror remove rename tmpfile tmpnam fileno
open close read write lseek stat fstat lstat mkdir rmdir unlink access
isalnum isalpha isblank iscntrl isdigit isgraph islower isprint ispunct isspace
isupper isxdigit tolower toupper
iswalnum iswalpha iswblank iswcntrl iswdigit iswgraph iswlower iswprint
iswpunct iswspace iswupper iswxdigit towlower towupper
wcslen wcscpy wcsncpy wcscat wcsncat wcscmp wcsncmp wcschr wcsrchr wcsstr
wmemcpy wmemmove wmemset wmemcmp wmemchr
mbrtowc wcrtomb mbstowcs wcstombs mbtowc wctomb mblen mbrlen mbsinit btowc wctob
setlocale localeconv
time clock difftime mktime gmtime localtime asctime ctime strftime
sin cos tan asin acos atan atan2 sinh cosh tanh exp log log10 pow sqrt ceil
floor fabs fmod frexp ldexp modf
setjmp longjmp signal raise
assert
_Getctype _Getpctype _Getpwctype _Getwctype _Getptype _Getcoll _Getwcoll
_Gettnames _Getmbcurmax _Getdateorder _Getcvt _Getlconv _Locksyslock
_Unlocksyslock _Lockit _Mbrtowc _Wcrtomb _Tolower _Toupper
__cxa_atexit __cxa_finalize __cxa_guard_acquire __cxa_guard_release
__cxa_guard_abort __cxa_begin_catch __cxa_end_catch __cxa_rethrow __cxa_throw
__cxa_allocate_exception __cxa_free_exception __cxa_pure_virtual __dynamic_cast
__cxa_demangle __cxa_thread_atexit __errno __error __stack_chk_fail
__stack_chk_guard __assert __assert_fail
_Unwind_Resume _Unwind_RaiseException _Unwind_DeleteException
__gxx_personality_v0 uncaught_exception uncaught_exceptions
sceLibcHeapGetTraceInfo sceLibcMspaceCreate sceLibcMspaceDestroy
sceLibcMspaceMalloc sceLibcMspaceFree sceLibcMspaceCalloc sceLibcMspaceRealloc
sceLibcMspaceMemalign sceLibcMspaceMallocUsableSize
sceLibcSetHeapAllocationCallback sceLibcOnce
_ZdlPv _ZdaPv _Znwm _Znam _ZdlPvm _ZdaPvm
catchReturnFromMain _init_env __libc_init __libc_fini
""".split()

# C++ operator new/delete'in TUM varyantlari (Itanium mangling). fJnpuVVBbKk'in
# _Znwm cikmasi, esinin de bu ailede olma ihtimalini yukseltiyor.
C += """
_ZdlPv _ZdaPv _Znwm _Znam _ZdlPvm _ZdaPvm
_ZnwmRKSt9nothrow_t _ZnamRKSt9nothrow_t _ZdlPvRKSt9nothrow_t _ZdaPvRKSt9nothrow_t
_ZnwmSt11align_val_t _ZnamSt11align_val_t
_ZdlPvSt11align_val_t _ZdaPvSt11align_val_t
_ZdlPvmSt11align_val_t _ZdaPvmSt11align_val_t
_ZnwmSt11align_val_tRKSt9nothrow_t _ZnamSt11align_val_tRKSt9nothrow_t
_ZdlPvSt11align_val_tRKSt9nothrow_t _ZdaPvSt11align_val_tRKSt9nothrow_t
_ZSt15get_new_handlerv _ZSt15set_new_handlerPFvvE _ZSt17__throw_bad_allocv
_ZSt9terminatev _ZSt13set_terminatePFvvE _ZSt14set_unexpectedPFvvE
_ZNSt9exceptionD1Ev _ZNSt9exceptionD2Ev _ZNKSt9exception4whatEv
_ZNSt13runtime_errorD1Ev _ZNSt12length_errorD1Ev
__cxa_bad_cast __cxa_bad_typeid __cxa_call_unexpected __cxa_current_exception_type
__cxa_get_globals __cxa_get_globals_fast __cxa_uncaught_exception
__cxa_uncaught_exceptions __cxa_deleted_virtual
__tls_get_addr __register_frame_info __deregister_frame_info
_Unwind_Backtrace _Unwind_GetIP _Unwind_SetIP _Unwind_GetCFA
""".split()

# Ek C/POSIX isimleri
C += """
snprintf_s sprintf_s vsnprintf_s strcpy_s strcat_s memcpy_s
strtok_r strsep basename dirname realpath getcwd chdir
nanosleep usleep sleep gettimeofday clock_gettime clock_getres
pthread_once pthread_key_create pthread_getspecific pthread_setspecific
fwprintf swprintf vswprintf wprintf
towctrans wctrans wctype iswctype
imaxabs imaxdiv strtoimax strtoumax
memrchr rawmemchr stpcpy stpncpy
_ZNSt3__18ios_base4initEPv
sceLibcMallocUsableSize malloc_usable_size mallopt mallinfo
reallocarray recallocarray freezero
""".split()

# --- 3) Bilinmeyenleri ara --------------------------------------------------
unknown = sys.argv[1:] or ["Nmtr628eA3A", "fJnpuVVBbKk"]
table = {}
for name in C:
    table.setdefault(nid(name), name)

for u in unknown:
    hit = table.get(u)
    print(f"{u} -> {hit if hit else 'BULUNAMADI'}")

print(f"\n({len(C)} aday isim denendi)")
