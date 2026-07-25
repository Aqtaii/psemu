import hashlib, base64, itertools
SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
def nid(n):
    return base64.b64encode(hashlib.sha1(n.encode()+SALT).digest()[:8][::-1]).decode().replace('/','-')[:11]

unknown = {"2HnmKiLmV6s","AEJdIVZTEmo","GfxAp9Xyiqs","P8F2oavZXtY","QW2jL1J5rwY",
           "hqi8yMOCmG0","iPBqs+YUUFw","p6LrHjIQMdk","rcQCUr0EaRU"}

cands = set()

# 1) MSVC/UCRT locale ic fonksiyonlari (_Getpctype dogrulanmis desen)
base = ["Getctype","Getpctype","Getpwctype","Getwctype","Getptype","Getcoll","Getwcoll",
        "Gettnames","Getmbcurmax","Getdateorder","Getcvt","Getlconv","Getcloc","Getdays",
        "Getmonths","Getfmt","Getwctypes","Getdst","Getzone","Gettimezone",
        "Locksyslock","Unlocksyslock","Lockit","Locinfo","Mbrtowc","Wcrtomb","Mbtowc",
        "Wctomb","Towlower","Towupper","Tolower","Toupper","Strcoll","Wcscoll","Strxfrm",
        "Wcsxfrm","LStrcoll","LStrxfrm","LTolower","LToupper","Atexit","Wctype","Iswctype",
        "Fiopen","Stoul","Stod","Stof","Stoll","Dtest","Dscale","Dnorm","Exp","Sinh","Cosh",
        "FCosh","FSinh","FExp","LCosh","LSinh","LExp","Feraise","Fpcomp","Dint","Dunscale"]
for b in base:
    cands.add("_" + b)
    cands.add("__" + b)

# 2) libc: genis karakter + siniflandirma + string
cands.update("""wcsstr wcsspn wcscspn wcspbrk wcstok wcsdup wcsnlen wcscasecmp wcsncasecmp
wcstol wcstoul wcstod wcstof wcstoll wcstoull wcsftime wcswidth wcwidth wcscoll wcsxfrm
swprintf vswprintf vfwprintf fwprintf fputws fgetws putwc getwc ungetwc
iswalnum iswblank iswcntrl iswgraph iswprint iswpunct iswxdigit iswlower iswupper
iswalpha iswdigit iswspace towctrans wctrans wctype iswctype btowc wctob mbsinit mbrlen
mbsnrtowcs wcsnrtombs mbsrtowcs wcsrtombs mbstowcs wcstombs mbrtowc wcrtomb
strlcat strlcpy strnlen strdup strndup memrchr memmem strcasecmp strncasecmp strcasestr
strsep strspn strcspn strpbrk strstr strtok strtok_r strcoll strxfrm strerror strsignal
qsort bsearch atoi atol atoll strtof strtold snprintf vsnprintf asprintf vasprintf
isalnum isalpha iscntrl isdigit isgraph islower isprint ispunct isspace isupper isxdigit
tolower toupper localeconv setlocale nl_langinfo""".split())

# 3) C++ ABI / Itanium mangled
cands.update("""__cxa_atexit __cxa_finalize __cxa_guard_acquire __cxa_guard_release
__cxa_begin_catch __cxa_end_catch __cxa_rethrow __cxa_throw __cxa_allocate_exception
__cxa_free_exception __cxa_pure_virtual __cxa_demangle __cxa_current_exception_type
__cxa_get_globals __cxa_get_globals_fast __cxa_bad_cast __cxa_bad_typeid
__dynamic_cast __gxx_personality_v0 _Unwind_Resume _Unwind_RaiseException
__stack_chk_fail abort atexit""".split())
cands.update(["_Znwm","_Znam","_ZdlPv","_ZdaPv","_ZdlPvm","_ZdaPvm","_ZnwmRKSt9nothrow_t",
 "_ZdlPvRKSt9nothrow_t","_ZnwmSt11align_val_t","_ZdlPvSt11align_val_t",
 "_ZSt9terminatev","_ZSt13set_terminatePFvvE","_ZSt17__throw_bad_allocv",
 "_ZSt18uncaught_exceptionv","_ZSt19uncaught_exceptionsv",
 "_ZNKSt9type_info4nameEv","_ZNSt9type_infoD1Ev","_ZNSt9type_infoD2Ev",
 "_ZNSt9exceptionD1Ev","_ZNSt9exceptionD2Ev","_ZNKSt9exception4whatEv",
 "_ZNSt13runtime_errorC1EPKc","_ZNSt13runtime_errorD1Ev","_ZNSt11logic_errorD1Ev",
 "_ZNSt12length_errorD1Ev","_ZNSt16invalid_argumentD1Ev","_ZNSt12out_of_rangeD1Ev"])
# libc++ locale facet'leri (std::__1)
for m in ["_ZNSt3__16localeC1Ev","_ZNSt3__16localeD1Ev","_ZNKSt3__16locale4nameEv",
          "_ZNSt3__16locale5facet17__on_zero_sharedEv","_ZNSt3__15ctypeIcE2idE",
          "_ZNSt3__15ctypeIwE2idE","_ZNSt3__111char_traitsIcE6lengthEPKc",
          "_ZNSt3__111char_traitsIDsE6lengthEPKDs","_ZNSt3__16__clocEv",
          "_ZNSt3__1L6__clocEv","_ZNSt3__112__next_primeEm"]:
    cands.add(m)

rev = {}
for c in cands: rev.setdefault(nid(c), c)
hits = {u: rev[u] for u in unknown if u in rev}
print("=== ESLESENLER ===")
for k, v in sorted(hits.items()): print(f"  {k} = {v}")
print(f"\n{len(hits)}/{len(unknown)} cozuldu")
print("kalan:", sorted(u for u in unknown if u not in rev))
print("aday sayisi:", len(cands))
