# Builds the typing mod as a 32-bit DPLAY.dll with Strawberry's x86_64 MinGW (-m32, no CRT).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force out | Out-Null

# 32-bit import libraries (stdcall names decorated here, undecorated in the DLL via -k)
@'
LIBRARY kernel32.dll
EXPORTS
VirtualProtect@16
VirtualAlloc@16
CreateFileA@28
ReadFile@20
WriteFile@20
CloseHandle@4
GetTickCount@0
FlushInstructionCache@12
GetCurrentProcess@0
GetModuleFileNameA@12
GetModuleHandleA@4
'@ | Set-Content -Encoding ascii out\kernel32.def
@'
LIBRARY user32.dll
EXPORTS
SetWindowLongA@12
CallWindowProcA@20
wvsprintfA@12
wsprintfA
'@ | Set-Content -Encoding ascii out\user32.def
@'
LIBRARY gdi32.dll
EXPORTS
CreateFontA@56
SelectObject@8
SetTextColor@8
SetBkColor@8
SetBkMode@8
TextOutA@20
GetTextExtentPoint32A@16
'@ | Set-Content -Encoding ascii out\gdi32.def
foreach ($n in 'kernel32', 'user32', 'gdi32') {
    dlltool -m i386 -f --32 -k -d "out\$n.def" -l "out\lib$n.a"
    if ($LASTEXITCODE) { throw "dlltool $n failed" }
}

@'
LIBRARY DPLAY
EXPORTS
    DirectPlayCreate = dplayx.DirectPlayCreate @1
    DirectPlayEnumerate = dplayx.DirectPlayEnumerateA @2
'@ | Set-Content -Encoding ascii out\dplay.def

gcc -m32 -march=i686 -mno-sse -O2 -Wall -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns `
    -fno-stack-protector -fno-asynchronous-unwind-tables -c tdmod.c -o out\tdmod.o
if ($LASTEXITCODE) { throw 'compile failed' }

ld -m i386pe --shared --enable-stdcall-fixup -e _DllMain@12 --subsystem windows --image-base 0x10000000 `
    -o out\DPLAY.dll out\tdmod.o out\dplay.def out\libkernel32.a out\libuser32.a out\libgdi32.a
if ($LASTEXITCODE) { throw 'link failed' }

objdump -p out\DPLAY.dll | Select-String -Pattern 'Magic|DLL Name|Forwarder|DirectPlay|relocation' | ForEach-Object { $_.Line.Trim() }
"built $((Get-Item out\DPLAY.dll).Length) bytes"
