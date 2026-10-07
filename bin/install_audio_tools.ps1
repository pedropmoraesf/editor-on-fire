param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$BinDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("eof-audio-tools-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

function Download-File([string]$Url, [string]$OutFile) {
    Write-Host "Downloading $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile -Headers @{ 'User-Agent' = 'EOF audio tools installer' }
    if(!(Test-Path $OutFile) -or ((Get-Item $OutFile).Length -lt 1024)) {
        throw "Download failed or returned an unexpectedly small file: $Url"
    }
}

function Copy-FirstMatch([string]$Root, [string]$Name, [bool]$Required = $true) {
    $file = Get-ChildItem -Path $Root -Recurse -File -Filter $Name | Select-Object -First 1
    if(!$file) {
        if($Required) { throw "Could not find $Name in downloaded package." }
        return $false
    }
    $dest = Join-Path $BinDir $Name
    Copy-Item -Force $file.FullName $dest
    Write-Host "Installed $Name"
    return $true
}

function Test-Executable([string]$Path, [string[]]$Arguments) {
    if(!(Test-Path $Path)) {
        return $false
    }
    try {
        $stdout = Join-Path $TempRoot (([System.IO.Path]::GetFileNameWithoutExtension($Path)) + '.stdout.txt')
        $stderr = Join-Path $TempRoot (([System.IO.Path]::GetFileNameWithoutExtension($Path)) + '.stderr.txt')
        $p = Start-Process -FilePath $Path -ArgumentList $Arguments -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        return ($p.ExitCode -eq 0)
    }
    catch {
        return $false
    }
}

function Install-CompatibleLame {
    # EOF uses LAME only as a command-line MP3 decoder before oggenc2 encodes
    # the resulting WAV.  Use the stable Win32 LAME 3.100 build from RareWares.
    # It predates the 3.100.1/4.0 switch to an external mpg123 decoder DLL,
    # avoiding STATUS_INVALID_IMAGE_FORMAT (0xc000007b) failures caused by a
    # mismatched libmpg123_0.dll on some Windows systems.
    $lameZip = Join-Path $TempRoot 'lame-compat.zip'
    $lameDir = Join-Path $TempRoot 'lame-compat'
    Download-File 'https://www.rarewares.org/files/mp3/lame3.100-20200409.zip' $lameZip
    Expand-Archive -Force $lameZip $lameDir

    Remove-Item -Force (Join-Path $BinDir 'lame.exe') -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $BinDir 'libmpg123_0.dll') -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $BinDir 'libmpg123-0.dll') -ErrorAction SilentlyContinue

    Copy-FirstMatch $lameDir 'lame.exe' $true | Out-Null

    $lameExe = Join-Path $BinDir 'lame.exe'
    if(!(Test-Executable $lameExe @('--version'))) {
        throw 'The installed Win32 LAME compatibility build still cannot start on this Windows installation.'
    }
    Write-Host 'LAME startup test: OK'
}

function Install-FFmpeg {
    # The DTX exporter needs FFmpeg to select/cut the 25-second preview and to
    # apply the two-second fade-in/fade-out.  Use Gyan's static Windows
    # "release essentials" ZIP, which includes libvorbis and requires no DLLs
    # beside ffmpeg.exe.  The filename is a stable alias to the current release.
    $ffmpegZip = Join-Path $TempRoot 'ffmpeg-release-essentials.zip'
    $ffmpegDir = Join-Path $TempRoot 'ffmpeg-release-essentials'
    Download-File 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip' $ffmpegZip
    Expand-Archive -Force $ffmpegZip $ffmpegDir

    $ffmpegExe = Join-Path $BinDir 'ffmpeg.exe'
    Remove-Item -Force $ffmpegExe -ErrorAction SilentlyContinue
    Copy-FirstMatch $ffmpegDir 'ffmpeg.exe' $true | Out-Null

    if(!(Test-Executable $ffmpegExe @('-version'))) {
        throw 'ffmpeg.exe was installed, but Windows could not start it.'
    }
    Write-Host 'FFmpeg startup test: OK'
}

try {
    Write-Host ""
    Write-Host "EOF Windows audio tools installer"
    Write-Host "Target: $BinDir"
    Write-Host ""

    $lameExe = Join-Path $BinDir 'lame.exe'
    $lameWorks = $false
    if(!$Force -and (Test-Path $lameExe)) {
        Write-Host 'Testing existing lame.exe...'
        $lameWorks = Test-Executable $lameExe @('--version')
        if($lameWorks) {
            Write-Host 'Existing lame.exe starts correctly; keeping it.'
        }
        else {
            Write-Host 'Existing lame.exe cannot start. Replacing it with the Win32 compatibility build.' -ForegroundColor Yellow
        }
    }
    if($Force -or !$lameWorks) {
        Install-CompatibleLame
    }

    # oggenc2: Win32/P4 build recommended by EOF's Windows build instructions.
    $oggExe = Join-Path $BinDir 'oggenc2.exe'
    $oggWorks = $false
    if(!$Force -and (Test-Path $oggExe)) {
        Write-Host 'Testing existing oggenc2.exe...'
        $oggWorks = Test-Executable $oggExe @('--version')
        if($oggWorks) {
            Write-Host 'Existing oggenc2.exe starts correctly; keeping it.'
        }
        else {
            Write-Host 'Existing oggenc2.exe cannot start. Reinstalling it.' -ForegroundColor Yellow
        }
    }
    if($Force -or !$oggWorks) {
        $oggZip = Join-Path $TempRoot 'oggenc2.zip'
        $oggDir = Join-Path $TempRoot 'oggenc2'
        Download-File 'https://www.rarewares.org/files/ogg/oggenc2.88-1.3.7-P4.zip' $oggZip
        Expand-Archive -Force $oggZip $oggDir
        Remove-Item -Force $oggExe -ErrorAction SilentlyContinue
        Copy-FirstMatch $oggDir 'oggenc2.exe' $true | Out-Null
        if(!(Test-Executable $oggExe @('--version'))) {
            throw 'oggenc2.exe was installed, but Windows could not start it.'
        }
        Write-Host 'oggenc2 startup test: OK'
    }

    # FFmpeg is independent of LAME/OggEnc.  EOF invokes it as an external
    # executable for DTX preview slicing/fades and other optional conversions.
    $ffmpegExe = Join-Path $BinDir 'ffmpeg.exe'
    $ffmpegWorks = $false
    if(!$Force -and (Test-Path $ffmpegExe)) {
        Write-Host 'Testing existing ffmpeg.exe...'
        $ffmpegWorks = Test-Executable $ffmpegExe @('-version')
        if($ffmpegWorks) {
            Write-Host 'Existing ffmpeg.exe starts correctly; keeping it.'
        }
        else {
            Write-Host 'Existing ffmpeg.exe cannot start. Reinstalling it.' -ForegroundColor Yellow
        }
    }
    if($Force -or !$ffmpegWorks) {
        Write-Host 'Installing FFmpeg. The download is roughly 100 MB and can take a while.'
        Install-FFmpeg
    }

    Write-Host ""
    Write-Host 'EOF audio tools are installed and passed startup tests:'
    Write-Host '  lame.exe     - MP3 decoding for new projects'
    Write-Host '  oggenc2.exe  - OGG encoding for new projects'
    Write-Host '  ffmpeg.exe   - DTX 25-second preview + fades and optional conversions'
    Write-Host ""
    Write-Host 'Optional: oggCat.exe + bgd.dll are only needed for some OGG manipulation/leading-silence operations.'
    Write-Host 'EOF upstream recommends Ogg Video Tools 0.8 (not 0.8a) for those files:'
    Write-Host 'https://sourceforge.net/projects/oggvideotools/files/oggvideotools-win32/oggvideotools-0.8-win32/'
    Write-Host ""
}
catch {
    Write-Host ""
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'The installer did not modify EOF source code. Check the message above and try again.'
    exit 1
}
finally {
    if(Test-Path $TempRoot) {
        Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
    }
}
