# psemu: kalan isimsiz NID'leri GENIS aday sozlugu ile cozmeyi dener.
# Ipucu: _Getpctype dogrulandi -> oyunun libc'si MSVC/UCRT tarzi ic isimler
# kullaniyor. Bu yuzden MSVC CRT ic fonksiyonlarina agirlik veriyoruz.
import hashlib
import base64
import itertools

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(n: str) -> str:
    h = hashlib.sha1(n.encode() + SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().replace("/", "-")[:11]


unknown = {"2HnmKiLmV6s", "GfxAp9Xyiqs", "P8F2oavZXtY", "QW2jL1J5rwY",
           "hqi8yMOCmG0", "iPBqs+YUUFw", "p6LrHjIQMdk", "rcQCUr0EaRU"}

cands = set()

# 1) MSVC/UCRT ic fonksiyonlari (_Xxx ve __Xxx)
msvc = """Getctype Getpctype Getpwctype Getwctype Getptype Getcoll Getwcoll Gettnames
Getmbcurmax Getdateorder Getcvt Getlconv Getcloc Getdays Getmonths Getfmt Getzone
Locksyslock Unlocksyslock Lockit Locinfo Mbrtowc Wcrtomb Mbtowc Wctomb Towlower
Towupper Tolower Toupper Strcoll Wcscoll Strxfrm Wcsxfrm LStrcoll LStrxfrm LTolower
LToupper Atexit Wctype Iswctype Fiopen Stoul Stod Stof Stoll Stoull Dtest Dscale
Dnorm Exp Sinh Cosh FCosh FSinh FExp LCosh LSinh LExp Feraise Fpcomp Dint Dunscale
Xtime_get_ticks Thrd_sleep Cnd_do_broadcast_at_thread_exit Mtx_init Mtx_lock
Query_perf_counter Query_perf_frequency Throw_C_error Throw_Cpp_error
Random_device Winerror_map Syserror_map Debugger_hook Xlength_error Xout_of_range
Xbad_alloc Xinvalid_argument Xoverflow_error Xruntime_error Smanip Isnan Isinf""".split()
for m in msvc:
    cands.add("_" + m)
    cands.add("__" + m)
    cands.add(m)

# 2) Guvenli (_s) ve genel libc
libc = """memcpy_s memmove_s strcpy_s strcat_s sprintf_s snprintf_s vsnprintf_s
wcscpy_s wcscat_s strtok_s _snprintf _vsnprintf _snwprintf _vsnwprintf
strlcpy strlcat strnlen strdup strndup memrchr memmem strcasestr
qsort_r bsearch_s atoll strtoimax strtoumax imaxabs
localtime_r gmtime_r mktime strftime time clock difftime
fflush fputs fputc puts putchar fprintf printf sscanf sprintf
setvbuf setbuf rewind ftello fseeko fileno feof ferror clearerr
posix_memalign aligned_alloc valloc reallocf malloc_usable_size""".split()
cands.update(libc)

# 3) C++ (Itanium) yaygin semboller
itan = ["_Znwm", "_Znam", "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm",
        "_ZSt9terminatev", "_ZSt17__throw_bad_allocv", "_ZSt18uncaught_exceptionv",
        "_ZSt19uncaught_exceptionsv", "_ZNKSt9type_info4nameEv",
        "__cxa_atexit", "__cxa_finalize", "__cxa_guard_acquire", "__cxa_guard_release",
        "__cxa_begin_catch", "__cxa_end_catch", "__cxa_throw", "__cxa_rethrow",
        "__cxa_allocate_exception", "__cxa_free_exception", "__cxa_pure_virtual",
        "__cxa_bad_cast", "__cxa_bad_typeid", "__dynamic_cast", "__gxx_personality_v0",
        "_Unwind_Resume", "_Unwind_RaiseException", "__stack_chk_fail"]
cands.update(itan)

rev = {}
for c in cands:
    rev.setdefault(nid(c), c)

hits = {u: rev[u] for u in unknown if u in rev}
print(f"aday sayisi: {len(cands)}")
print("=== ESLESENLER ===")
for k, v in sorted(hits.items()):
    print(f"  {k} = {v}")
print(f"\n{len(hits)}/{len(unknown)} cozuldu")
print("kalan:", sorted(u for u in unknown if u not in hits))
