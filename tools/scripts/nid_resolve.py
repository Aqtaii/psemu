# psemu: bilinmeyen NID'leri aday sembol isimleriyle eslestirir.
# NID = base64(sha1(isim)[:8]), '/' -> '-', ilk 11 karakter (Sony formati).
import hashlib, base64

def nid(name: str) -> str:
    b = base64.b64encode(hashlib.sha1(name.encode()).digest()[:8]).decode()
    return b.replace('/', '-')[:11]

# 1) Algoritma dogrulamasi
assert nid("scePthreadMutexLock") == "9UK1vLZQft4", nid("scePthreadMutexLock")
print("algoritma DOGRULANDI (scePthreadMutexLock -> 9UK1vLZQft4)")

unknown = ["2HnmKiLmV6s","9nf8joUTSaQ","AEJdIVZTEmo","GfxAp9Xyiqs","Noj9PsJrsa8",
           "P8F2oavZXtY","Q1BL70XVV0o","QJ5xVfKkni0","QW2jL1J5rwY","SfQIZcqvvms",
           "fnUEjBCNRVU","hMAe+TWS9mQ","hqi8yMOCmG0","iPBqs+YUUFw","kALvdgEv5ME",
           "p6LrHjIQMdk","rcQCUr0EaRU"]

# 2) Aday isimler: MSVC/UCRT locale-ic fonksiyonlari + libc + C++ std
cands = []
for n in """_Getctype _Getpctype _Getpwctype _Getwctype _Getptype _Getcoll _Getwcoll
_Gettnames _Getmbcurmax _Getdateorder _Getcvt _Getlconv _Locksyslock _Unlocksyslock
_Lockit _Mbrtowc _Wcrtomb _Mbtowc _Wctomb _Mbstowcs _Wcstombs _Towlower _Towupper
_Tolower _Toupper _Strcoll _Wcscoll _Strxfrm _Wcsxfrm _LStrcoll _LStrxfrm _LTolower
_LToupper setlocale localeconv _Atomic_load _Atomic_store __cxa_guard_acquire
__cxa_guard_release __cxa_begin_catch __cxa_end_catch __cxa_rethrow __cxa_throw
__cxa_allocate_exception __cxa_free_exception __cxa_pure_virtual __dynamic_cast
_Unwind_Resume __gxx_personality_v0 mbrtowc wcrtomb wcslen wcscmp wcsncmp wcschr
wcsrchr wcscpy wcsncpy wcscat towlower towupper iswspace iswalpha iswdigit
btowc wctob mbsinit mbrlen wmemcpy wmemcmp wmemset""".split():
    cands.append(n)

# 3) C++ (Itanium) mangled adaylar: char_traits<char16_t> ve u16string uyeleri
for m in ["_ZNSt3__111char_traitsIDsE4findEPKDsmRS2_",
          "_ZNSt3__111char_traitsIDsE4moveEPDsPKDsm",
          "_ZNSt3__111char_traitsIDsE7compareEPKDsS3_m",
          "_ZNSt3__111char_traitsIDsE6lengthEPKDs",
          "_ZNSt3__111char_traitsIDsE4copyEPDsPKDsm",
          "_ZNSt3__111char_traitsIDsE6assignEPDsmDs"]:
    cands.append(m)

rev = {}
for c in cands:
    rev[nid(c)] = c

print("\n=== ESLESENLER ===")
hit = 0
for u in unknown:
    if u in rev:
        print(f"  {u} = {rev[u]}")
        hit += 1
print(f"\n{hit}/{len(unknown)} cozuldu")
