$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out=Join-Path $root 'build\native-atomic-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
function Slice([string]$path,[string]$start,[string]$end) {
 $s=[IO.File]::ReadAllText($path);$a=$s.IndexOf($start);$b=$s.IndexOf($end,$a+1)
 if($a -lt 0 -or $b -le $a){throw "Production extraction failed: $start"}
 return $s.Substring($a,$b-$a)
}
$dir="$root\gpu\src\graphics\shader\recompiler"
$source=[IO.File]::ReadAllText("$root\tests\storage_1d_lowering_test.cpp")
$source+=Slice "$dir\ResourceMaterialization.cpp" 'Decoder::ImageDimension DescriptorDimension(' 'bool NullImageDescriptor('
$source+=Slice "$dir\spirvEmitter\spirvEmitterImageHelpers.cpp" 'uint32_t EmitImageCoordU32(' 'uint32_t EmitImageLoadCoordU32('
$infoPath="$root\gpu\src\graphics\host_gpu\renderer\imageInfo.h"
$infoText=[IO.File]::ReadAllText($infoPath)
$source+=[regex]::Match($infoText,'enum class StorageSampledViewShape : uint8_t \{[^}]+\};').Value
$source+="[[nodiscard]] inline constexpr StorageSampledViewShape`n"+(Slice $infoPath 'SelectStorageSampledViewShape(' '[[nodiscard]] inline constexpr bool IsSupportedRenderTargetElementSize(')
$source+='using IR::ImageResource;using IR::DescriptorBindingKind;'
$source+=Slice "$dir\BindingLayout.cpp" 'bool ImageBinding(' 'bool CollectValue('
$source+=@'
int main() {
 Check(SelectStorageSampledViewShape(8,1,1,1,1)==StorageSampledViewShape::Image2D);
 for(auto values:{std::array<uint32_t,4>{2,1,1,1},{1,2,1,1},{1,1,2,1},{1,1,1,2},{1,1,1,0}})
  Check(SelectStorageSampledViewShape(8,values[0],values[1],values[2],values[3])==StorageSampledViewShape::Unsupported);
 Check(SelectStorageSampledViewShape(9,1,1,8,8)==StorageSampledViewShape::Image2D);

 DescriptorValue descriptor;
 descriptor.dwords={0x6e829200,0x04d00001,0,0x81b00fac,0,0x00700000,0,0};
 Check(DescriptorDimension(descriptor,ResourceKind::StorageImageUint)==Decoder::ImageDimension::Dim1D);
 Check(DescriptorDimension(descriptor,ResourceKind::StorageImage)==Decoder::ImageDimension::Dim1D);
 // The existing sampled path is deliberately preserved.
 Check(DescriptorDimension(descriptor,ResourceKind::Image)==Decoder::ImageDimension::Dim2D);
 for(auto kind:{ResourceKind::StorageImage,ResourceKind::StorageImageUint}) {
  IR::ImageResource image;image.kind=kind;image.dimension=Decoder::ImageDimension::Dim1D;
  IR::DescriptorBindingKind binding;
  Check(ImageBinding(image,binding));
  Check(binding==(kind==ResourceKind::StorageImage?IR::DescriptorBindingKind::Storage2D:IR::DescriptorBindingKind::StorageUint2D));
 }
 for(auto dim:{Decoder::ImageDimension::Dim1D,Decoder::ImageDimension::Dim2D}) {
  for(uint32_t components:{1u,2u,3u}) {
   EmitterState state;IR::Instruction inst;
   inst.memory.image_dimension=dim;inst.memory.image_address_components=components;
   EmitImageCoordU32(&state,inst,ImageViewKind::Dim2D);
   const bool read_y=dim!=Decoder::ImageDimension::Dim1D && components>1;
   Check(state.reads==(read_y?2u:1u));
   Check(state.builder.result[0]==7 && state.builder.result[1]==(read_y?0xdeadbeefu:0u));
  }
 }
 std::puts("PASS: captured 1D descriptor, float/uint binding, poisoned adjacent VGPR and 2D preservation");
}
'@
$file=Join-Path $out 'storage_1d_production.cpp'
[IO.File]::WriteAllText($file,$source,[Text.UTF8Encoding]::new($false))
Push-Location $out
try {
 & clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DFMT_HEADER_ONLY=1 "/I$root\gpu\src" "/I$root\gpu\generated" "/I$root\gpu\3rdparty\fmt\include" "/I$root\gpu\3rdparty\magic_enum\include" $file /Fe:storage_1d_test.exe
 if($LASTEXITCODE -ne 0){throw '1D storage test compilation failed'}
 & .\storage_1d_test.exe
 if($LASTEXITCODE -ne 0){throw '1D storage regression failed'}
} finally {Pop-Location}
