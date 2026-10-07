using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml.Linq;
using Newtonsoft.Json.Linq;
using RocksmithToolkitLib;
using RocksmithToolkitLib.DLCPackage;
using RocksmithToolkitLib.DLCPackage.AggregateGraph;
using RocksmithToolkitLib.DLCPackage.Manifest2014.Tone;
using RocksmithToolkitLib.Sng;
using RocksmithToolkitLib.ToolkitTone;
using RocksmithToolkitLib.XML;

/*
 * EOF PSARC helper
 * ----------------
 * This program is intentionally built into the user's installed Rocksmith
 * Custom Song Toolkit directory.  The installed RSToolkit release is treated
 * as the runtime: DDC, Template/, packer.exe and RocksmithToolkitLib resources
 * are used in-place instead of copying a partial toolkit into EOF.
 *
 * Audio conversion and final package generation intentionally mirror the
 * working Python generator supplied for this feature:
 *   - ddc\ddc.exe is invoked directly when DD is enabled;
 *   - WAV files are copied to Template\Originals\SFX;
 *   - the installed Template.wproj is processed by WwiseCLI.exe;
 *   - AkCopyStreamedFiles.exe is invoked using SoundbanksInfo.xml;
 *   - generated WEMs are read from Template\.cache\Windows\SFX;
 *   - eof_psarc_packer_override.cs serializes the .dlc.xml and invokes the
 *     installed packer.exe with -b -t ... -o ... .
 */
namespace EofPsarcHelper
{
    internal static class Program
    {
        private static string SafeKey(string text)
        {
            if (String.IsNullOrEmpty(text)) return "EOF_Tone";
            var chars = text.Where(Char.IsLetterOrDigit).ToArray();
            var value = new String(chars);
            return String.IsNullOrEmpty(value) ? "EOF_Tone" : "EOF_" + value;
        }

        private static List<T> NewList<T>() { return new List<T>(); }

        private static string Quote(string value)
        {
            return "\"" + (value ?? String.Empty).Replace("\"", "\\\"") + "\"";
        }

        private static void RunTool(string executable, string arguments, string workingDirectory, string step)
        {
            if (String.IsNullOrEmpty(executable) || !File.Exists(executable))
                throw new FileNotFoundException(step + " executable was not found.", executable);

            Console.WriteLine(step + ": " + executable + " " + arguments);
            var psi = new ProcessStartInfo(executable, arguments)
            {
                WorkingDirectory = String.IsNullOrEmpty(workingDirectory) ? Environment.CurrentDirectory : workingDirectory,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };

            string stdout, stderr;
            int exitCode;
            using (var process = Process.Start(psi))
            {
                stdout = process.StandardOutput.ReadToEnd();
                stderr = process.StandardError.ReadToEnd();
                process.WaitForExit();
                exitCode = process.ExitCode;
            }

            if (!String.IsNullOrWhiteSpace(stdout)) Console.WriteLine(stdout.Trim());
            if (!String.IsNullOrWhiteSpace(stderr)) Console.Error.WriteLine(stderr.Trim());
            if (exitCode != 0)
                throw new InvalidOperationException(step + " failed with exit code " + exitCode + ".");
        }

        private static ToolkitPedal FindPedal(IList<ToolkitPedal> pedals, bool bass, params string[] needles)
        {
            foreach (var needle in needles)
            {
                var p = pedals.FirstOrDefault(x => x.Bass == bass &&
                    ((x.Key ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0 ||
                     (x.Name ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0 ||
                     (x.Category ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0));
                if (p != null) return p;
            }
            foreach (var needle in needles)
            {
                var p = pedals.FirstOrDefault(x =>
                    ((x.Key ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0 ||
                     (x.Name ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0 ||
                     (x.Category ?? "").IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0));
                if (p != null) return p;
            }
            return null;
        }

        private static string ToneDescriptorFor(string token, bool bass)
        {
            var value = (token ?? "clean").ToLowerInvariant();
            string name;
            if (value.IndexOf('+') >= 0 || value.IndexOf(',') >= 0 || value.IndexOf(';') >= 0)
                name = "MULTI_EFFECT";
            else if (value.Contains("overdrive")) name = "OVERDRIVE";
            else if (value.Contains("distortion") || value.Contains("drive")) name = "DISTORTION";
            else if (value.Contains("fuzz")) name = "FUZZ";
            else if (value.Contains("chorus")) name = "CHORUS";
            else if (value.Contains("flanger")) name = "FLANGER";
            else if (value.Contains("phaser")) name = "PHASER";
            else if (value.Contains("tremolo")) name = "TREMOLO";
            else if (value.Contains("delay")) name = "DELAY";
            else if (value.Contains("reverb")) name = "REVERB";
            else if (value.Contains("wah")) name = "FILTER";
            else if (value.Contains("acoustic")) name = "ACOUSTIC";
            else if (bass) name = "BASS";
            else name = "CLEAN";

            var descriptor = ToneDescriptor.List().FirstOrDefault(x => x.Name == name);
            return descriptor != null ? descriptor.Descriptor : "$[35720]CLEAN";
        }

        private static Tone2014 MakeSuggestedTone(string name, string token, bool bass)
        {
            var pedals = ToolkitPedal.LoadFromResource(GameVersion.RS2014);
            var tone = new Tone2014
            {
                Name = name,
                Key = SafeKey(name),
                IsCustom = true,
                Volume = -12.0f,
                ToneDescriptors = new List<string>()
            };

            ToolkitPedal amp = null, cab = null;
            if (!bass)
            {
                amp = pedals.FirstOrDefault(p => p.Key == "Amp_OrangeAD50");
                cab = pedals.FirstOrDefault(p => p.Key == "Cab_OrangeJimmyBean_57_Cone");
            }
            if (amp == null)
                amp = pedals.FirstOrDefault(p => p.TypeEnum == PedalType.Amp && p.Bass == bass) ?? pedals.FirstOrDefault(p => p.TypeEnum == PedalType.Amp);
            if (cab == null)
                cab = pedals.FirstOrDefault(p => p.TypeEnum == PedalType.Cabinet && p.Bass == bass) ?? pedals.FirstOrDefault(p => p.TypeEnum == PedalType.Cabinet);
            if (amp != null) tone.GearList.Amp = (Pedal2014)amp.MakePedalSetting(GameVersion.RS2014);
            if (cab != null) tone.GearList.Cabinet = (Pedal2014)cab.MakePedalSetting(GameVersion.RS2014);

            var parts = (token ?? "clean").ToLowerInvariant().Split(new[] { '+', ',', ';' }, StringSplitOptions.RemoveEmptyEntries);
            int preSlot = 1, rackSlot = 1;
            foreach (var raw in parts)
            {
                var part = raw.Trim();
                ToolkitPedal p = null;
                switch (part)
                {
                    case "drive":
                    case "distortion": p = FindPedal(pedals, bass, "VintageDistortion", "SuperDrive", "Distortion"); break;
                    case "overdrive": p = FindPedal(pedals, bass, "SuperDrive", "Overdrive", "Drive"); break;
                    case "fuzz": p = FindPedal(pedals, bass, "Fuzz"); break;
                    case "chorus": p = FindPedal(pedals, bass, "StudioChorus", "DigitalChorus", "Chorus"); break;
                    case "flanger": p = FindPedal(pedals, bass, "Flanger"); break;
                    case "phaser": p = FindPedal(pedals, bass, "Phaser", "Phase"); break;
                    case "tremolo": p = FindPedal(pedals, bass, "Tremolo"); break;
                    case "delay": p = FindPedal(pedals, bass, "Delay", "Echo"); break;
                    case "reverb": p = FindPedal(pedals, bass, "Reverb"); break;
                    case "wah": p = FindPedal(pedals, bass, "Wah"); break;
                    case "clean":
                    case "acoustic":
                    default: break;
                }
                if (p == null) continue;
                var setting = (Pedal2014)p.MakePedalSetting(GameVersion.RS2014);
                if (p.TypeEnum == PedalType.Rack)
                {
                    if (rackSlot <= 4) tone.GearList["Rack" + rackSlot++] = setting;
                }
                else if (p.TypeEnum == PedalType.Pedal)
                {
                    if (preSlot <= 4) tone.GearList["PrePedal" + preSlot++] = setting;
                }
            }

            tone.ToneDescriptors.Add(ToneDescriptorFor(token, bass));
            return tone;
        }

        private static Tone2014 BuildTone(JObject jt, bool bass, string keySeed)
        {
            var name = (string)jt["name"] ?? "Default";
            var source = (string)jt["source"] ?? "";
            Tone2014 tone = null;
            if (!String.IsNullOrEmpty(source) && File.Exists(source))
            {
                /* RSToolkit's importer handles RS2014 JSON manifests, packages
                 * (including PSARC) and Rocksmith profile databases. */
                var imported = Tone2014.Import(source);
                tone = imported.FirstOrDefault();
            }
            if (tone == null)
                tone = MakeSuggestedTone(name, (string)jt["suggestion"] ?? "clean", bass);
            tone.Name = name;
            tone.Key = SafeKey(keySeed);
            if (tone.ToneDescriptors == null) tone.ToneDescriptors = new List<string>();
            return tone;
        }



        private static void ForceArrangementRoleXml(string xmlPath, string role)
        {
            if (String.IsNullOrEmpty(xmlPath) || !File.Exists(xmlPath) || String.IsNullOrEmpty(role) || role == "auto") return;
            var doc = XDocument.Load(xmlPath, LoadOptions.PreserveWhitespace);
            var arrangement = doc.Descendants().FirstOrDefault(x => x.Name.LocalName == "arrangement");
            var props = doc.Descendants().FirstOrDefault(x => x.Name.LocalName == "arrangementProperties");
            var alt = role == "alt_rhythm" || role == "alt_lead";
            var bass = role == "bass";
            var lead = role == "lead" || role == "alt_lead";
            var rhythm = role == "rhythm" || role == "alt_rhythm";
            if (arrangement != null) arrangement.Value = bass ? "Bass" : (lead ? "Lead" : "Rhythm");
            if (props != null)
            {
                props.SetAttributeValue("pathBass", bass ? "1" : "0");
                props.SetAttributeValue("pathLead", lead ? "1" : "0");
                props.SetAttributeValue("pathRhythm", rhythm ? "1" : "0");
                props.SetAttributeValue("bonusArr", alt ? "1" : "0");
                props.SetAttributeValue("represent", alt ? "0" : "1");
            }
            doc.Save(xmlPath, SaveOptions.DisableFormatting);
        }

        private static void ApplyArrangementRoles(JObject spec)
        {
            foreach (JObject ja in (JArray)spec["arrangements"])
                ForceArrangementRoleXml((string)ja["xml"], (string)ja["role"] ?? "auto");
        }

        private static void RewriteArrangementToneRefs(string xmlPath, IDictionary<string, string> rename)
        {
            if (String.IsNullOrEmpty(xmlPath) || !File.Exists(xmlPath) || rename == null || rename.Count == 0) return;
            var doc = XDocument.Load(xmlPath, LoadOptions.PreserveWhitespace);
            var changed = false;
            foreach (var element in doc.Descendants())
            {
                var local = element.Name.LocalName.ToLowerInvariant();
                string replacement;
                if ((local == "tonebase" || local == "tonea" || local == "toneb" || local == "tonec" || local == "toned") &&
                    rename.TryGetValue(element.Value, out replacement))
                {
                    element.Value = replacement;
                    changed = true;
                }
                if (local == "tone")
                {
                    var attr = element.Attribute("name");
                    if (attr != null && rename.TryGetValue(attr.Value, out replacement))
                    {
                        attr.Value = replacement;
                        changed = true;
                    }
                }
            }
            if (changed) doc.Save(xmlPath, SaveOptions.DisableFormatting);
        }

        private static Arrangement BuildArrangement(JObject ja, List<Tone2014> tones)
        {
            var xml = (string)ja["xml"];
            var trackId = (int?)ja["track"] ?? 0;
            var role = (string)ja["role"] ?? "auto";
            ForceArrangementRoleXml(xml, role);
            var song = Song2014.LoadFromFile(xml);
            var type = Song2014.DetectArrangementType(xml);
            var arr = new Arrangement
            {
                SongXml = new SongXML { File = xml, Version = song.Version },
                SongFile = new SongFile { File = "" },
                ArrangementType = type,
                ArrangementPropeties = song.ArrangementProperties,
                ScrollSpeed = 20,
                TuningStrings = song.Tuning,
                CapoFret = song.Capo,
                TuningPitch = 440.0,
                Represent = song.ArrangementProperties != null && song.ArrangementProperties.Represent == 1,
                BonusArr = song.ArrangementProperties != null && song.ArrangementProperties.BonusArr == 1,
                XmlComments = Song2014.ReadXmlComments(xml)
            };

            var lower = (song.Arrangement ?? "").ToLowerInvariant();
            if (type == ArrangementType.Bass)
            {
                arr.ArrangementName = ArrangementName.Bass;
                arr.RouteMask = RouteMask.Bass;
                arr.PluckedType = (song.ArrangementProperties != null && song.ArrangementProperties.BassPick == 1) ? PluckedType.Picked : PluckedType.NotPicked;
            }
            else
            {
                if (lower.Contains("combo"))
                {
                    arr.ArrangementName = ArrangementName.Combo;
                    arr.RouteMask = RouteMask.Lead;
                }
                else if (lower.Contains("rhythm") || (song.ArrangementProperties != null && song.ArrangementProperties.PathRhythm == 1))
                {
                    arr.ArrangementName = ArrangementName.Rhythm;
                    arr.RouteMask = RouteMask.Rhythm;
                }
                else
                {
                    arr.ArrangementName = ArrangementName.Lead;
                    arr.RouteMask = RouteMask.Lead;
                }
            }

            var toneArray = (JArray)ja["tones"];
            if (toneArray != null && toneArray.Count > 0)
            {
                var t0 = BuildTone((JObject)toneArray[0], type == ArrangementType.Bass, "T" + trackId + "_0_" + ((string)((JObject)toneArray[0])["name"] ?? "Default"));
                tones.Add(t0);
                arr.ToneBase = t0.Key;
                arr.ToneA = t0.Key;

                var rename = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                var raw0 = (string)((JObject)toneArray[0])["name"] ?? "Default";
                rename[raw0] = t0.Key;

                if (toneArray.Count > 1)
                {
                    var raw1 = (string)((JObject)toneArray[1])["name"] ?? "Tone 2";
                    var t1 = BuildTone((JObject)toneArray[1], type == ArrangementType.Bass, "T" + trackId + "_1_" + raw1);
                    tones.Add(t1);
                    arr.ToneB = t1.Key;
                    rename[raw1] = t1.Key;
                }
                RewriteArrangementToneRefs(xml, rename);
            }
            return arr;
        }

        private static void ShiftArrangementXml(string xmlPath, double seconds)
        {
            if (seconds <= 0.0) return;
            var inv = CultureInfo.InvariantCulture;
            var doc = XDocument.Load(xmlPath, LoadOptions.PreserveWhitespace);
            foreach (var e in doc.Descendants())
            {
                foreach (var a in e.Attributes().ToList())
                {
                    var n = a.Name.LocalName;
                    if (n != "time" && n != "startTime" && n != "endTime") continue;
                    double value;
                    if (Double.TryParse(a.Value, NumberStyles.Float, inv, out value))
                        a.Value = (value + seconds).ToString("0.000", inv);
                }

                double scalar;
                if (e.Name.LocalName == "songLength" && Double.TryParse(e.Value, NumberStyles.Float, inv, out scalar))
                    e.Value = (scalar + seconds).ToString("0.000", inv);
                else if (e.Name.LocalName == "startBeat" && Double.TryParse(e.Value, NumberStyles.Float, inv, out scalar))
                    e.Value = (scalar + seconds).ToString("0.000", inv);
                else if (e.Name.LocalName == "offset" && Double.TryParse(e.Value, NumberStyles.Float, inv, out scalar))
                    e.Value = (scalar - seconds).ToString("0.000", inv);
            }
            doc.Save(xmlPath, SaveOptions.DisableFormatting);
        }

        private static void ApplyAudioPrefixToArrangements(JObject spec)
        {
            var prefixMs = (long?)spec["prefixMs"] ?? 0L;
            if (prefixMs <= 0L) return;
            var seconds = prefixMs / 1000.0;
            Console.WriteLine("EOF prefix sync: shifting staged arrangement XML by " + seconds.ToString("0.000", CultureInfo.InvariantCulture) + " seconds.");
            foreach (JObject ja in (JArray)spec["arrangements"])
                ShiftArrangementXml((string)ja["xml"], seconds);
        }

        private static void ApplyDDC(JObject spec)
        {
            if (!((bool?)spec["dynamicDifficulty"] ?? true)) return;
            var toolkitRoot = AppDomain.CurrentDomain.SetupInformation.ApplicationBase;
            var ddc = Path.Combine(toolkitRoot, "ddc", "ddc.exe");
            if (!File.Exists(ddc))
                throw new FileNotFoundException("Dynamic difficulty is enabled, but the installed RSToolkit ddc\\ddc.exe was not found.", ddc);

            foreach (JObject ja in (JArray)spec["arrangements"])
            {
                var xml = (string)ja["xml"];
                RunTool(ddc, Quote(Path.GetFileName(xml)) + " -p Y -t N", Path.GetDirectoryName(xml), "RSToolkit DDC");
            }
        }



        private static void CreateHotPreviewWav(JObject spec, string source, string dest)
        {
            var prefixMs = (long?)spec["prefixMs"] ?? 0L;
            var musicLengthMs = (long?)spec["musicLengthMs"] ?? 0L;
            try
            {
                using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var br = new BinaryReader(input, Encoding.ASCII))
                {
                    if (Encoding.ASCII.GetString(br.ReadBytes(4)) != "RIFF") throw new InvalidDataException("Not RIFF");
                    br.ReadUInt32();
                    if (Encoding.ASCII.GetString(br.ReadBytes(4)) != "WAVE") throw new InvalidDataException("Not WAVE");
                    ushort format = 0, channels = 0, blockAlign = 0, bits = 0;
                    uint rate = 0, byteRate = 0;
                    long dataOffset = -1, dataBytes = 0;
                    while (input.Position + 8 <= input.Length)
                    {
                        var id = Encoding.ASCII.GetString(br.ReadBytes(4));
                        var size = br.ReadUInt32();
                        var payload = input.Position;
                        var available = Math.Max(0L, Math.Min((long)size, input.Length - payload));
                        if (id == "fmt " && available >= 16)
                        {
                            format = br.ReadUInt16();
                            channels = br.ReadUInt16();
                            rate = br.ReadUInt32();
                            byteRate = br.ReadUInt32();
                            blockAlign = br.ReadUInt16();
                            bits = br.ReadUInt16();
                        }
                        else if (id == "data")
                        {
                            dataOffset = payload;
                            dataBytes = available;
                        }
                        var next = payload + (long)size + ((size & 1U) != 0U ? 1L : 0L);
                        input.Position = Math.Min(next, input.Length);
                        if (dataOffset >= 0 && format != 0) break;
                    }
                    if (format != 1 || channels == 0 || rate == 0 || byteRate == 0 || blockAlign == 0 || bits == 0 || dataOffset < 0 || dataBytes <= 0)
                        throw new InvalidDataException("Unsupported staged WAV format");

                    var musicStart = prefixMs;
                    var hotOffset = musicLengthMs > 40000L ? (long)(musicLengthMs * 0.30) : 0L;
                    var startMs = musicStart + hotOffset;
                    var remainingMusic = Math.Max(0L, musicLengthMs - hotOffset);
                    var previewMs = Math.Min(25000L, remainingMusic > 0L ? remainingMusic : 25000L);
                    var startByte = (startMs * (long)byteRate) / 1000L;
                    startByte -= startByte % blockAlign;
                    if (startByte >= dataBytes) startByte = 0;
                    var wanted = (previewMs * (long)byteRate) / 1000L;
                    wanted -= wanted % blockAlign;
                    var copyBytes = Math.Min(wanted, dataBytes - startByte);
                    if (copyBytes <= 0) throw new InvalidDataException("Empty preview range");

                    input.Position = dataOffset + startByte;
                    using (var output = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None))
                    using (var bw = new BinaryWriter(output, Encoding.ASCII))
                    {
                        bw.Write(Encoding.ASCII.GetBytes("RIFF"));
                        bw.Write((uint)(36L + copyBytes));
                        bw.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
                        bw.Write((uint)16);
                        bw.Write(format);
                        bw.Write(channels);
                        bw.Write(rate);
                        bw.Write(byteRate);
                        bw.Write(blockAlign);
                        bw.Write(bits);
                        bw.Write(Encoding.ASCII.GetBytes("data"));
                        bw.Write((uint)copyBytes);
                        var buffer = new byte[65536];
                        var left = copyBytes;
                        while (left > 0)
                        {
                            var request = (int)Math.Min((long)buffer.Length, left);
                            var got = input.Read(buffer, 0, request);
                            if (got <= 0) throw new EndOfStreamException();
                            output.Write(buffer, 0, got);
                            left -= got;
                        }
                    }
                    Console.WriteLine("PSARC preview: DTX 30% window, no fades; start " + startMs + " ms, length " + ((copyBytes * 1000L) / byteRate) + " ms.");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("PSARC preview crop fallback: " + ex.Message);
                File.Copy(source, dest, true);
            }
        }

        private static string ConvertWwiseLikePython(JObject spec, string wav)
        {
            var toolkitRoot = AppDomain.CurrentDomain.SetupInformation.ApplicationBase.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var templateRoot = Path.Combine(toolkitRoot, "Template");
            var project = Path.Combine(templateRoot, "Template.wproj");
            var originals = Path.Combine(templateRoot, "Originals", "SFX");
            var cache = Path.Combine(templateRoot, ".cache", "Windows", "SFX");
            var generated = Path.Combine(templateRoot, "GeneratedSoundBanks", "Windows");
            var info = Path.Combine(generated, "SoundbanksInfo.xml");

            if (!File.Exists(project))
                throw new FileNotFoundException("RSToolkit Template\\Template.wproj was not found.", project);

            var wwiseCli = (string)spec["wwise"];
            if (String.IsNullOrEmpty(wwiseCli) || !File.Exists(wwiseCli))
                throw new FileNotFoundException("WwiseCLI.exe was not found.", wwiseCli);
            var wwiseBin = Path.GetDirectoryName(wwiseCli);
            var akCopy = Path.Combine(wwiseBin, "tools", "AkCopyStreamedFiles.exe");
            if (!File.Exists(akCopy))
                throw new FileNotFoundException("AkCopyStreamedFiles.exe was not found under the selected Wwise bin\\tools folder.", akCopy);

            var previewWav = Path.Combine(Path.GetDirectoryName(wav), "psarc_audio_preview.wav");
            CreateHotPreviewWav(spec, wav, previewWav);

            Directory.CreateDirectory(originals);
            Directory.CreateDirectory(cache);
            Directory.CreateDirectory(generated);

            foreach (var file in Directory.EnumerateFiles(originals, "*", SearchOption.TopDirectoryOnly))
            {
                try { File.Delete(file); } catch { }
            }
            foreach (var file in Directory.EnumerateFiles(cache, "*", SearchOption.TopDirectoryOnly))
            {
                try { File.Delete(file); } catch { }
            }

            var templateMain = Path.Combine(originals, "Audio.wav");
            var templatePreview = Path.Combine(originals, "Audio_preview.wav");
            File.Copy(wav, templateMain, true);
            File.Copy(previewWav, templatePreview, true);

            var wwiseArgs = Quote(project) +
                " -GenerateSoundBanks -Platform Windows -Language \"English(US)\" -NoWwiseDat -ClearAudioFileCache -Save";
            RunTool(wwiseCli, wwiseArgs, toolkitRoot, "Wwise CLI");

            if (!File.Exists(info))
                throw new FileNotFoundException("Wwise completed, but RSToolkit Template\\GeneratedSoundBanks\\Windows\\SoundbanksInfo.xml was not created.", info);

            var copyArgs = "-info " + Quote(info) +
                " -outputpath " + Quote(generated) +
                " -banks \"Template Init\" -languages \"English(US)\"";
            RunTool(akCopy, copyArgs, toolkitRoot, "AkCopyStreamedFiles");

            var wemFiles = Directory.EnumerateFiles(cache, "*.wem", SearchOption.TopDirectoryOnly).ToList();
            var mainSource = wemFiles.FirstOrDefault(path =>
                Path.GetFileName(path).StartsWith("Audio", StringComparison.OrdinalIgnoreCase) &&
                !Path.GetFileName(path).StartsWith("Audio_preview", StringComparison.OrdinalIgnoreCase));
            var previewSource = wemFiles.FirstOrDefault(path =>
                Path.GetFileName(path).StartsWith("Audio_preview", StringComparison.OrdinalIgnoreCase));

            if (String.IsNullOrEmpty(mainSource) || !File.Exists(mainSource))
                throw new FileNotFoundException("Wwise did not generate the main Audio*.wem in RSToolkit Template\\.cache\\Windows\\SFX.");
            if (String.IsNullOrEmpty(previewSource) || !File.Exists(previewSource))
                throw new FileNotFoundException("Wwise did not generate Audio_preview*.wem in RSToolkit Template\\.cache\\Windows\\SFX.");

            var wem = Path.ChangeExtension(wav, ".wem");
            var previewWem = Path.Combine(Path.GetDirectoryName(wem), Path.GetFileNameWithoutExtension(wem) + "_preview.wem");
            File.Copy(mainSource, wem, true);
            File.Copy(previewSource, previewWem, true);
            Console.WriteLine("Wwise WEM: " + wem);
            Console.WriteLine("Wwise preview WEM: " + previewWem);
            return wem;
        }

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length < 1) throw new ArgumentException("Usage: eof_psarc_helper.exe <spec.json>");
                var spec = JObject.Parse(File.ReadAllText(args[0]));

                var wav = (string)spec["wav"];
                if (String.IsNullOrEmpty(wav) || !File.Exists(wav))
                    throw new FileNotFoundException("Prepared WAV was not found: " + wav);

                var specDir = Path.GetDirectoryName(Path.GetFullPath(args[0]));
                var albumArt = Directory.EnumerateFiles(specDir, "album_art.*", SearchOption.TopDirectoryOnly).FirstOrDefault();
                if (String.IsNullOrEmpty(albumArt) || !File.Exists(albumArt))
                    throw new FileNotFoundException("Album artwork was not found in the EOF PSARC staging folder.");

                ApplyAudioPrefixToArrangements(spec);
                ApplyArrangementRoles(spec);
                ApplyDDC(spec);
                var wem = ConvertWwiseLikePython(spec, wav);
                var previewWem = Path.Combine(Path.GetDirectoryName(wem), Path.GetFileNameWithoutExtension(wem) + "_preview.wem");

                var allTones = NewList<Tone2014>();
                var arrangements = NewList<Arrangement>();
                foreach (JObject ja in (JArray)spec["arrangements"])
                    arrangements.Add(BuildArrangement(ja, allTones));

                var uniqueTones = NewList<Tone2014>();
                foreach (var t in allTones)
                    if (!uniqueTones.Any(x => x.Key == t.Key)) uniqueTones.Add(t);

                var data = new DLCPackageData
                {
                    GameVersion = GameVersion.RS2014,
                    Pc = true,
                    AppId = "248750",
                    Name = (string)spec["dlcKey"],
                    SongInfo = new SongInfo
                    {
                        Artist = (string)spec["artist"],
                        ArtistSort = (string)spec["artist"],
                        SongDisplayName = (string)spec["title"],
                        SongDisplayNameSort = (string)spec["title"],
                        Album = (string)spec["album"],
                        AlbumSort = (string)spec["album"],
                        SongYear = (int?)spec["year"] ?? 2026,
                        AverageTempo = (int?)spec["averageTempo"] ?? 120
                    },
                    AlbumArtPath = albumArt,
                    Arrangements = arrangements,
                    TonesRS2014 = uniqueTones,
                    OggPath = wem,
                    OggPreviewPath = File.Exists(previewWem) ? previewWem : wem,
                    OggQuality = 4,
                    Volume = -7.0f,
                    PreviewVolume = -5.0f,
                    DefaultShowlights = true,
                    ToolkitInfo = new ToolkitInfo
                    {
                        PackageAuthor = "EOF",
                        PackageVersion = (string)spec["version"] ?? "1",
                        PackageComment = "Generated by EOF using installed Rocksmith Custom Song Toolkit tools",
                        PackageRating = "5"
                    }
                };

                var platform = new Platform(GamePlatform.Pc, GameVersion.RS2014);
                var output = (string)spec["output"];
                var generatedPsarc = DLCPackageCreator.Generate(output, data, platform, DLCPackageType.Song, 1);
                Console.WriteLine(generatedPsarc);
                return File.Exists(generatedPsarc) ? 0 : 3;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(ex.ToString());
                return 2;
            }
        }
    }
}
