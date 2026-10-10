$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source=[IO.File]::ReadAllText((Join-Path $root 'gpu\src\libs\libFont.cpp'))
$source=[regex]::Replace($source,'(?m)^#include "(common/|libs/|loader/)[^\r\n]+','')
$source=[regex]::Replace($source,'(?m)^LIB_VERSION\([^\r\n]+','')
$source=$source.Substring(0,$source.IndexOf('LIB_DEFINE(InitFont_1)'))+"`n} // Libs`n"
$prefix=@"
#define KYTY_SYSV_ABI __attribute__((sysv_abi))
#define PRINT_NAME() ((void)0)
#define LOGF(...) ((void)0)
namespace Libs { constexpr int OK=0; }
"@
$tests=@"
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 using namespace Libs::Font;
 std::ifstream input("C:/Windows/Fonts/arial.ttf",std::ios::binary); CHECK(input);
 std::vector<unsigned char> font_data((std::istreambuf_iterator<char>(input)),{});CHECK(font_data.size()>1000);
 FontLibrary library=nullptr;CHECK(FontCreateLibraryWithEdition(nullptr,nullptr,0,&library)==0 && library);
 FontHandle handle=nullptr; CHECK(FontOpenFontMemory(library,font_data.data(),static_cast<uint32_t>(font_data.size()),nullptr,&handle)==0 && handle);
 CHECK(FontSetScalePixel(handle,24.0f,32.0f)==0);
 void* glyph=nullptr; CHECK(FontGenerateCharGlyph(handle,'A',nullptr,&glyph)==0 && glyph);
 int previous=-1;CHECK(FontGlyphDefineAttribute(glyph,7,&previous)==0 && previous==0);
 std::vector<unsigned char> surface_bytes(128*128+16,0);std::fill(surface_bytes.end()-16,surface_bytes.end(),0xa5);
 FontRenderSurface surface{};FontRenderSurfaceInit(&surface,surface_bytes.data(),128,1,128,128);
 FontGlyphMetrics metrics{};FontRenderResult result{};
 CHECK(FontRenderCharGlyphImageHorizontal(handle,'A',&surface,8.0f,40.0f,&metrics,&result)==0);
 CHECK(result.trans_image && result.trans_image->address && reinterpret_cast<uintptr_t>(result.trans_image->address)>65536);
 CHECK(result.trans_image->image_width>0 && result.trans_image->image_height>0);
 CHECK(std::any_of(surface_bytes.begin(),surface_bytes.end()-16,[](unsigned char b){return b!=0;}));
 CHECK(std::all_of(surface_bytes.end()-16,surface_bytes.end(),[](unsigned char b){return b==0xa5;}));
 CHECK(FontDeleteGlyph(nullptr,&glyph)==0 && glyph==nullptr);
 CHECK(FontCloseFont(handle)==0);CHECK(FontDestroyLibrary(&library)==0 && library==nullptr);
 std::puts("PASS: actual font handles, SysV float arguments, glyph bitmap pixels, render result pointers and surface bounds");
}
"@
$out=Join-Path $root 'build\native-atomic-tests';New-Item -ItemType Directory -Path $out -Force | Out-Null
$cpp=Join-Path $out 'font_test.cpp';$exe=Join-Path $out 'font_test.exe'
[IO.File]::WriteAllText($cpp,"$prefix`n$source`n$tests",[Text.UTF8Encoding]::new($false))
& clang-cl /nologo /std:c++20 /Od /EHsc /MD "/I$root\gpu\3rdparty\stb" $cpp "/Fe:$exe" "/Fo:$out\font_test.obj"
if($LASTEXITCODE -ne 0){throw 'Font test compilation failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Font test failed'}