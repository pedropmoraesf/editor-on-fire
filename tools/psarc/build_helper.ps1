param(
    [Parameter(Mandatory=$true)][string]$ToolkitRoot,
    [string]$Source = ""
)

$ErrorActionPreference = 'Stop'
$BuildLog = Join-Path $env:TEMP 'eof_psarc_build.log'
$GeneratedSource = Join-Path $env:TEMP 'eof_psarc_helper_build.cs'
$GeneratedPackerSource = Join-Path $env:TEMP 'eof_psarc_packer_build.cs'
try { Remove-Item -LiteralPath $BuildLog -Force -ErrorAction SilentlyContinue } catch { }
try { Remove-Item -LiteralPath $GeneratedSource -Force -ErrorAction SilentlyContinue } catch { }
try { Remove-Item -LiteralPath $GeneratedPackerSource -Force -ErrorAction SilentlyContinue } catch { }

trap {
    $message = ($_ | Out-String)
    try {
        Add-Content -LiteralPath $BuildLog -Value $message -Encoding UTF8
    } catch { }
    try { Remove-Item -LiteralPath $GeneratedSource -Force -ErrorAction SilentlyContinue } catch { }
    try { Remove-Item -LiteralPath $GeneratedPackerSource -Force -ErrorAction SilentlyContinue } catch { }
    [Console]::Error.WriteLine($message)
    exit 1
}

# EOF historically stores the selected RSToolkit directory with a trailing
# backslash.  When C system() constructs a quoted Windows command line, a final
# backslash immediately before the closing quote can escape that quote and
# swallow the following "-Source" argument into ToolkitRoot.  Strip anything
# from that marker onward; Source is self-resolving below, so this remains safe.
$sourceMarker = $ToolkitRoot.IndexOf(' -Source ')
if ($sourceMarker -ge 0) {
    $ToolkitRoot = $ToolkitRoot.Substring(0, $sourceMarker)
    $Source = ""
}
$ToolkitRoot = $ToolkitRoot.Trim().Trim('"').TrimEnd('\','/')
if ([String]::IsNullOrWhiteSpace($ToolkitRoot)) {
    throw 'RSToolkit folder argument is empty.'
}
$ToolkitRoot = [IO.Path]::GetFullPath($ToolkitRoot)

if ([String]::IsNullOrWhiteSpace($Source)) {
    $Source = Join-Path $PSScriptRoot 'eof_psarc_helper.cs'
}
$Source = $Source.Trim().Trim('"')
$Source = [IO.Path]::GetFullPath($Source)

if (!(Test-Path $ToolkitRoot -PathType Container)) { throw "RSToolkit folder not found: $ToolkitRoot" }
if (!(Test-Path $Source -PathType Leaf)) { throw "EOF PSARC helper source not found: $Source" }

$PsarcDir = Split-Path -Parent $Source
$PackerOverride = Join-Path $PsarcDir 'eof_psarc_packer_override.cs'
$HotPreviewSource = Join-Path $PsarcDir 'eof_psarc_hot_preview.cs'
if (!(Test-Path $PackerOverride -PathType Leaf)) {
    throw "EOF RSToolkit packer adapter source not found: $PackerOverride"
}
if (!(Test-Path $HotPreviewSource -PathType Leaf)) {
    throw "EOF PSARC hot-preview source not found: $HotPreviewSource"
}

# Keep the long-lived helper source compatible with older PSARC branches while
# routing this build to the same hottest-window policy used by the DTX exporter.
# The generated source exists only for the compiler and is deleted afterwards.
$helperText = [IO.File]::ReadAllText($Source).Replace("`r`n", "`n")
$previewCall = 'CreateHotPreviewWav(spec, wav, previewWav);'
if ($helperText.IndexOf($previewCall, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate the PSARC preview call in eof_psarc_helper.cs.'
}
$helperText = $helperText.Replace($previewCall, 'HotPreview.Create(spec, wav, previewWav);')

# The bundled bass preset is extracted from a known-good RS2014 manifest.  Keep
# its original BASS Name/Key pair instead of converting it into an EOF-generated
# key.  Rocksmith's stock bass tone binding is sensitive to this identity.
$oldToneTail = @'
            if (tone == null)
                tone = MakeSuggestedTone(name, (string)jt["suggestion"] ?? "clean", bass);
            tone.Name = name;
            tone.Key = SafeKey(keySeed);
            if (tone.ToneDescriptors == null) tone.ToneDescriptors = new List<string>();
            return tone;
'@
$oldToneTail = $oldToneTail.Replace('\"', '"').Replace("`r`n", "`n")
$newToneTail = @'
            var bundledBass = bass && !String.IsNullOrEmpty(source) &&
                String.Equals(Path.GetFileName(source), "default_bass_tone.json", StringComparison.OrdinalIgnoreCase);
            if (tone == null)
                tone = MakeSuggestedTone(name, (string)jt["suggestion"] ?? "clean", bass);
            if (bundledBass)
            {
                tone.Name = "BASS";
                tone.Key = "BASS";
            }
            else
            {
                tone.Name = name;
                tone.Key = SafeKey(keySeed);
            }
            if (tone.ToneDescriptors == null) tone.ToneDescriptors = new List<string>();
            return tone;
'@
$newToneTail = $newToneTail.Replace('\"', '"').Replace("`r`n", "`n")
if ($helperText.IndexOf($oldToneTail, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate BuildTone tail for bundled bass preservation.'
}
$helperText = $helperText.Replace($oldToneTail, $newToneTail)

$buildArrangementMarker = '        private static Arrangement BuildArrangement(JObject ja, List<Tone2014> tones)'
$bassHelpers = @'
        private static bool IsBundledBassSpec(JObject ja)
        {
            if (ja == null) return false;
            var toneArray = ja["tones"] as JArray;
            if (toneArray == null || toneArray.Count == 0) return false;
            var source = (string)((JObject)toneArray[0])["source"] ?? String.Empty;
            return !String.IsNullOrEmpty(source) &&
                String.Equals(Path.GetFileName(source), "default_bass_tone.json", StringComparison.OrdinalIgnoreCase);
        }

        private static void StripBundledBassToneXml(string xmlPath)
        {
            if (String.IsNullOrEmpty(xmlPath) || !File.Exists(xmlPath)) return;
            var doc = XDocument.Load(xmlPath, LoadOptions.PreserveWhitespace);
            var removeNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                "tones", "tonebase", "tonea", "toneb", "tonec", "toned"
            };
            foreach (var node in doc.Descendants().Where(x => removeNames.Contains(x.Name.LocalName)).ToList())
                node.Remove();
            doc.Save(xmlPath, SaveOptions.DisableFormatting);
        }

'@
$bassHelpers = $bassHelpers.Replace('\"', '"').Replace("`r`n", "`n")
if ($helperText.IndexOf($buildArrangementMarker, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate BuildArrangement for bundled bass patch.'
}
$helperText = $helperText.Replace($buildArrangementMarker, $bassHelpers + $buildArrangementMarker)

$oldArrangementHead = @'
            var xml = (string)ja["xml"];
            var trackId = (int?)ja["track"] ?? 0;
            var role = (string)ja["role"] ?? "auto";
            ForceArrangementRoleXml(xml, role);
            var song = Song2014.LoadFromFile(xml);
'@
$oldArrangementHead = $oldArrangementHead.Replace('\"', '"').Replace("`r`n", "`n")
$newArrangementHead = @'
            var xml = (string)ja["xml"];
            var trackId = (int?)ja["track"] ?? 0;
            var role = (string)ja["role"] ?? "auto";
            var bundledBass = String.Equals(role, "bass", StringComparison.OrdinalIgnoreCase) && IsBundledBassSpec(ja);
            ForceArrangementRoleXml(xml, role);
            if (bundledBass)
                StripBundledBassToneXml(xml);
            var song = Song2014.LoadFromFile(xml);
'@
$newArrangementHead = $newArrangementHead.Replace('\"', '"').Replace("`r`n", "`n")
if ($helperText.IndexOf($oldArrangementHead, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate BuildArrangement header for bundled bass patch.'
}
$helperText = $helperText.Replace($oldArrangementHead, $newArrangementHead)

$toneArrayMarker = '            var toneArray = (JArray)ja["tones"];'.Replace('\"', '"')
$singleBassBlock = @'
            var toneArray = (JArray)ja["tones"];
            if (bundledBass && toneArray != null && toneArray.Count > 0)
            {
                var bassTone = BuildTone((JObject)toneArray[0], true, "BASS");
                if (!tones.Any(x => x != null && String.Equals(x.Key, "BASS", StringComparison.OrdinalIgnoreCase)))
                    tones.Add(bassTone);
                arr.ToneBase = "BASS";
                arr.ToneA = null;
                arr.ToneB = null;
                arr.ToneC = null;
                arr.ToneD = null;
                Console.WriteLine("Rocksmith bass preset: exact single BASS tone bound; alternate tone slots disabled.");
                return arr;
            }
'@
$singleBassBlock = $singleBassBlock.Replace('\"', '"').Replace("`r`n", "`n")
if ($helperText.IndexOf($toneArrayMarker, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate arrangement tone array for bundled bass patch.'
}
$helperText = $helperText.Replace($toneArrayMarker, $singleBassBlock)
[IO.File]::WriteAllText($GeneratedSource, $helperText, (New-Object Text.UTF8Encoding($false)))

# The packer adapter normally gives every tone a song-specific display name.
# Preserve BASS/BASS for this one bundled stock-compatible preset so the final
# manifest matches the known-good reference instead of becoming "Clean Bass".
$packerText = [IO.File]::ReadAllText($PackerOverride).Replace("`r`n", "`n")
$displayMarker = '                var display = UniqueToneName(songTitle, EffectName(tone, oldName, bass), used);'
$bassBindingBlock = @'
                if (bass && String.Equals(key, "BASS", StringComparison.OrdinalIgnoreCase) &&
                    String.Equals(oldName, "BASS", StringComparison.OrdinalIgnoreCase))
                {
                    foreach (var arrangement in info.Arrangements)
                    {
                        if (arrangement == null || !ArrangementIsBass(arrangement)) continue;
                        if (ToneReferenceMatches(arrangement.ToneBase, tone, oldName) ||
                            ToneReferenceMatches(arrangement.ToneA, tone, oldName) ||
                            ToneReferenceMatches(arrangement.ToneB, tone, oldName) ||
                            ToneReferenceMatches(arrangement.ToneC, tone, oldName) ||
                            ToneReferenceMatches(arrangement.ToneD, tone, oldName))
                        {
                            arrangement.ToneBase = "BASS";
                            arrangement.ToneA = null;
                            arrangement.ToneB = null;
                            arrangement.ToneC = null;
                            arrangement.ToneD = null;
                        }
                    }
                    tone.Name = "BASS";
                    tone.Key = "BASS";
                    used.Add("BASS");
                    Console.WriteLine("Rocksmith bass preset binding preserved: Name=BASS, Key=BASS, ToneBase=BASS.");
                    continue;
                }
                var display = UniqueToneName(songTitle, EffectName(tone, oldName, bass), used);
'@
$bassBindingBlock = $bassBindingBlock.Replace('\"', '"').Replace("`r`n", "`n")
if ($packerText.IndexOf($displayMarker, [StringComparison]::Ordinal) -lt 0) {
    throw 'Could not locate tone display binding in eof_psarc_packer_override.cs.'
}
$packerText = $packerText.Replace($displayMarker, $bassBindingBlock)
[IO.File]::WriteAllText($GeneratedPackerSource, $packerText, (New-Object Text.UTF8Encoding($false)))

# Match the deployment model used by the user's working Python generator: EOF
# does not supply a partial RSToolkit runtime.  The complete, pre-installed
# RSToolkit release is the runtime and all managed references come from the same
# directory as RocksmithToolkitGUI.exe / packer.exe.
$requiredFiles = @(
    'RocksmithToolkitLib.dll',
    'packer.exe',
    'Template\Template.wproj'
)
foreach ($name in $requiredFiles) {
    $path = Join-Path $ToolkitRoot $name
    if (!(Test-Path $path -PathType Leaf)) {
        throw "Required RSToolkit file not found: $path"
    }
}
$ddc = Join-Path $ToolkitRoot 'ddc\ddc.exe'
if (!(Test-Path $ddc -PathType Leaf)) {
    throw "Required RSToolkit DDC executable not found: $ddc"
}

$candidates = @(
    "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\csc.exe",
    "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
)
$csc = $candidates | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
if ($null -eq $csc) { throw "Microsoft .NET Framework C# compiler (csc.exe) was not found." }

$managedNames = @(
    'RocksmithToolkitLib.dll',
    'ICSharpCode.SharpZipLib.dll',
    'MiscUtil.dll',
    'Newtonsoft.Json.dll',
    'X360.dll',
    'zlib.net.dll'
)

$refs = New-Object System.Collections.Generic.List[string]
foreach ($name in $managedNames) {
    $installed = Join-Path $ToolkitRoot $name
    if (!(Test-Path $installed -PathType Leaf)) {
        throw "Required RSToolkit managed dependency not found: $installed"
    }
    $refs.Add('/reference:' + $installed)
}

@(
    'System.Data.dll',
    'System.Drawing.dll',
    'System.Runtime.Serialization.dll',
    'System.Windows.Forms.dll',
    'System.Xml.dll',
    'System.Xml.Linq.dll',
    'Microsoft.CSharp.dll'
) | ForEach-Object { $refs.Add('/reference:' + $_) }

# Build the helper directly into the installed RSToolkit directory.  Running
# there makes RocksmithToolkitLib.Extensions.ExternalApps.TOOLKIT_ROOT resolve
# to the real installation, so DDC, Template and packer resources are the same
# ones used by the toolkit and by the proven Python workflow.
$out = Join-Path $ToolkitRoot 'eof_psarc_helper.exe'
$compilerArgs = New-Object System.Collections.Generic.List[string]
$compilerArgs.Add('/nologo')
$compilerArgs.Add('/target:exe')
$compilerArgs.Add('/optimize+')
$compilerArgs.Add('/platform:x86')
$compilerArgs.Add('/out:' + $out)
foreach ($r in $refs) { $compilerArgs.Add($r) }
$compilerArgs.Add($GeneratedSource)
$compilerArgs.Add($HotPreviewSource)
$compilerArgs.Add($GeneratedPackerSource)

$compilerOutput = & $csc @compilerArgs 2>&1
$compilerOutput | Out-File -LiteralPath $BuildLog -Append -Encoding UTF8
$compilerExit = $LASTEXITCODE
try { Remove-Item -LiteralPath $GeneratedSource -Force -ErrorAction SilentlyContinue } catch { }
try { Remove-Item -LiteralPath $GeneratedPackerSource -Force -ErrorAction SilentlyContinue } catch { }
if ($compilerExit -ne 0 -or !(Test-Path $out -PathType Leaf)) {
    throw "Could not build eof_psarc_helper.exe (csc exit code $compilerExit). See $BuildLog"
}

# Remove stale files left by the older EOF/bin bootstrap design.  They are not
# used anymore and could make troubleshooting ambiguous after an upgrade.
foreach ($stale in @('eof_psarc_worker.dll', 'eof_psarc_dependencies.txt')) {
    $stalePath = Join-Path $ToolkitRoot $stale
    try { Remove-Item -LiteralPath $stalePath -Force -ErrorAction SilentlyContinue } catch { }
}

Write-Output $out
