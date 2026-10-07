using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;
using RocksmithToolkitLib;
using RocksmithToolkitLib.DLCPackage;
using RocksmithToolkitLib.DLCPackage.Manifest2014.Tone;
using X360.STFS;

/*
 * EOF PSARC packer adapter
 * ------------------------
 * The user's known-good Python workflow writes a normal RSToolkit .dlc.xml and
 * invokes the installed packer.exe with:
 *
 *   packer.exe -b -t <template.dlc.xml> -o <output.psarc>
 *
 * Keep that exact packaging path.  PC and Mac are deliberately serialized and
 * built in separate packer.exe processes.  RSToolkit's packer uses temporary
 * album-art DDS files while building; asking one invocation to build both
 * desktop targets can delete/reuse that temporary DDS between targets and ends
 * with the "tmp\\*.dds not found" failure seen in EOF's export log.
 */
namespace EofPsarcHelper
{
    internal static class DLCPackageCreator
    {
        private static readonly HashSet<string> LowerCaseTitleWords = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "a", "o", "as", "os", "um", "uma", "uns", "umas", "de", "da", "do", "das", "dos",
            "em", "no", "na", "nos", "nas", "ao", "aos", "à", "às", "por", "para", "com", "sem",
            "sob", "sobre", "entre", "e", "ou", "mas", "que",
            "an", "the", "and", "or", "but", "for", "nor", "on", "at", "to", "from", "by", "in",
            "of", "with", "as", "vs", "via",
            "el", "la", "los", "las", "uno", "una", "unos", "unas", "del", "al", "en", "con", "sin",
            "y", "pero",
            "le", "les", "une", "des", "du", "au", "aux", "avec", "sans", "pour", "par", "et",
            "il", "lo", "i", "gli", "uno", "di", "della", "dei", "delle", "da", "su", "per", "tra", "fra",
            "der", "die", "das", "den", "dem", "des", "ein", "eine", "einer", "eines", "einem", "einen",
            "und", "oder", "aber", "von", "zu", "zum", "zur", "mit", "ohne", "für", "im", "am", "auf", "an",
            "de", "het", "een", "van", "voor", "met", "zonder", "op", "aan", "en", "of"
        };

        private static string SmartTitleCase(string value)
        {
            if (String.IsNullOrWhiteSpace(value)) return String.Empty;
            var lower = value.Trim().ToLowerInvariant();
            const string pattern = @"\p{L}[\p{L}\p{M}'’]*|\p{N}+";
            var matches = Regex.Matches(lower, pattern);
            var wordIndex = 0;
            var totalWords = matches.Count;
            return Regex.Replace(lower, pattern, delegate(Match match)
            {
                var word = match.Value;
                var index = wordIndex++;
                var keepLower = index > 0 && index < totalWords - 1 && LowerCaseTitleWords.Contains(word);
                if (keepLower || String.IsNullOrEmpty(word)) return word;
                return Char.ToUpperInvariant(word[0]) + word.Substring(1);
            });
        }

        private static string DescriptorEffectName(Tone2014 tone)
        {
            if (tone == null || tone.ToneDescriptors == null) return String.Empty;
            foreach (var descriptor in tone.ToneDescriptors)
            {
                var value = descriptor ?? String.Empty;
                var bracket = value.LastIndexOf(']');
                if (bracket >= 0 && bracket + 1 < value.Length)
                    value = value.Substring(bracket + 1);
                value = value.Replace('_', ' ').Trim();
                if (String.IsNullOrEmpty(value)) continue;
                if (String.Equals(value, "BASS", StringComparison.OrdinalIgnoreCase)) return "Clean bass";
                if (String.Equals(value, "MULTI EFFECT", StringComparison.OrdinalIgnoreCase)) return "Multi effect";
                return value;
            }
            return String.Empty;
        }

        private static string EffectName(Tone2014 tone, string oldName, bool bass)
        {
            var effect = (oldName ?? String.Empty).Trim().Replace('_', ' ');
            effect = Regex.Replace(effect, @"^(EOF|AI)\s+", String.Empty, RegexOptions.IgnoreCase).Trim();
            effect = Regex.Replace(effect, @"^Modulation\s+", String.Empty, RegexOptions.IgnoreCase).Trim();
            var generic = effect.ToLowerInvariant();
            if (generic == "" || generic == "default" || generic == "tone 2" || generic == "tone2" ||
                generic == "alternate" || generic == "bass default" || generic == "bass alternate" ||
                generic == "guitar default" || generic == "guitar alternate")
            {
                effect = DescriptorEffectName(tone);
                if (String.IsNullOrEmpty(effect)) effect = bass ? "Clean bass" : "Clean";
            }
            if (bass && String.Equals(effect, "Bass", StringComparison.OrdinalIgnoreCase))
                effect = "Clean bass";
            return SmartTitleCase(effect);
        }

        private static string UniqueToneName(string songTitle, string effect, HashSet<string> used)
        {
            var baseName = SmartTitleCase(songTitle) + " - " + SmartTitleCase(effect);
            var candidate = baseName;
            var suffix = 2;
            while (used.Contains(candidate)) candidate = baseName + " " + suffix++;
            used.Add(candidate);
            return candidate;
        }

        private static bool ArrangementIsBass(object arrangement)
        {
            if (arrangement == null) return false;
            var typeProperty = arrangement.GetType().GetProperty("ArrangementType");
            if (typeProperty != null)
            {
                var value = typeProperty.GetValue(arrangement, null);
                if (value != null && value.ToString().IndexOf("Bass", StringComparison.OrdinalIgnoreCase) >= 0)
                    return true;
            }
            var nameProperty = arrangement.GetType().GetProperty("ArrangementName");
            if (nameProperty != null)
            {
                var value = nameProperty.GetValue(arrangement, null);
                if (value != null && value.ToString().IndexOf("Bass", StringComparison.OrdinalIgnoreCase) >= 0)
                    return true;
            }
            return false;
        }

        private static void RewriteArrangementXmlToneNames(string xmlPath, IDictionary<string, string> rename)
        {
            if (String.IsNullOrEmpty(xmlPath) || !File.Exists(xmlPath) || rename == null || rename.Count == 0)
                return;
            var doc = XDocument.Load(xmlPath, LoadOptions.PreserveWhitespace);
            var changed = false;
            foreach (var element in doc.Descendants())
            {
                string replacement;
                foreach (var attr in element.Attributes().ToList())
                {
                    if (rename.TryGetValue(attr.Value, out replacement))
                    {
                        attr.Value = replacement;
                        changed = true;
                    }
                }
                if (!element.HasElements && rename.TryGetValue(element.Value, out replacement))
                {
                    element.Value = replacement;
                    changed = true;
                }
            }
            if (changed) doc.Save(xmlPath, SaveOptions.DisableFormatting);
        }

        /* Rocksmith 2014 uses ToneBase for the default tone and ToneA for the
         * first alternate tone.  Older EOF helper builds duplicated ToneBase in
         * ToneA and put the real alternate in ToneB.  Normalize the slots first;
         * the following name pass then converts every reference to Tone2014.Name,
         * which is what RSToolkit's 2014 manifest builder actually resolves. */
        private static void NormalizeRocksmithToneSlots(DLCPackageData info)
        {
            if (info == null || info.Arrangements == null) return;
            foreach (var arrangement in info.Arrangements)
            {
                if (arrangement == null) continue;
                var baseKey = arrangement.ToneBase;
                var toneA = arrangement.ToneA;
                var toneB = arrangement.ToneB;

                if (!String.IsNullOrWhiteSpace(toneB) &&
                    (String.IsNullOrWhiteSpace(toneA) || String.Equals(toneA, baseKey, StringComparison.OrdinalIgnoreCase)))
                {
                    arrangement.ToneA = toneB;
                    arrangement.ToneB = null;
                }
                else if (!String.IsNullOrWhiteSpace(toneA) &&
                         String.Equals(toneA, baseKey, StringComparison.OrdinalIgnoreCase))
                {
                    arrangement.ToneA = null;
                }

                Console.WriteLine("Rocksmith tone slots before name binding: Base=" + (arrangement.ToneBase ?? "<none>") +
                                  ", A=" + (arrangement.ToneA ?? "<none>") +
                                  ", B=" + (arrangement.ToneB ?? "<none>"));
            }
        }

        private static bool ToneReferenceMatches(string value, Tone2014 tone, string oldName)
        {
            if (String.IsNullOrWhiteSpace(value) || tone == null) return false;
            if (!String.IsNullOrWhiteSpace(tone.Key) && String.Equals(value, tone.Key, StringComparison.OrdinalIgnoreCase))
                return true;
            return !String.IsNullOrWhiteSpace(oldName) && String.Equals(value, oldName, StringComparison.OrdinalIgnoreCase);
        }

        private static void NormalizeRocksmithToneNames(DLCPackageData info)
        {
            if (info == null || info.TonesRS2014 == null || info.Arrangements == null) return;
            var songTitle = info.SongInfo != null ? info.SongInfo.SongDisplayName : info.Name;
            if (String.IsNullOrWhiteSpace(songTitle)) songTitle = "Song";
            var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var tone in info.TonesRS2014)
            {
                if (tone == null) continue;
                var key = tone.Key ?? String.Empty;
                var oldName = tone.Name ?? String.Empty;
                var bass = info.Arrangements.Any(a => a != null && ArrangementIsBass(a) &&
                    (ToneReferenceMatches(a.ToneBase, tone, oldName) ||
                     ToneReferenceMatches(a.ToneA, tone, oldName) ||
                     ToneReferenceMatches(a.ToneB, tone, oldName) ||
                     ToneReferenceMatches(a.ToneC, tone, oldName) ||
                     ToneReferenceMatches(a.ToneD, tone, oldName)));
                var display = UniqueToneName(songTitle, EffectName(tone, oldName, bass), used);
                var rename = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                if (!String.IsNullOrWhiteSpace(oldName)) rename[oldName] = display;
                if (!String.IsNullOrWhiteSpace(key)) rename[key] = display;

                foreach (var arrangement in info.Arrangements)
                {
                    if (arrangement == null) continue;
                    if (ToneReferenceMatches(arrangement.ToneBase, tone, oldName)) arrangement.ToneBase = display;
                    if (ToneReferenceMatches(arrangement.ToneA, tone, oldName)) arrangement.ToneA = display;
                    if (ToneReferenceMatches(arrangement.ToneB, tone, oldName)) arrangement.ToneB = display;
                    if (ToneReferenceMatches(arrangement.ToneC, tone, oldName)) arrangement.ToneC = display;
                    if (ToneReferenceMatches(arrangement.ToneD, tone, oldName)) arrangement.ToneD = display;
                    if (arrangement.SongXml != null && !String.IsNullOrWhiteSpace(arrangement.SongXml.File))
                        RewriteArrangementXmlToneNames(arrangement.SongXml.File, rename);
                }

                /* RSToolkit 2014 Attributes.GetToneName() resolves arrangement
                 * ToneBase/ToneA/... by Tone2014.Name, not Tone2014.Key.  Keep
                 * Key as the unique package identifier, but bind all arrangement
                 * references to this display Name before packer serialization. */
                tone.Name = display;
                Console.WriteLine("Rocksmith tone binding: " + oldName + " / " + key + " -> " + display);
            }

            foreach (var arrangement in info.Arrangements)
            {
                if (arrangement == null) continue;
                Console.WriteLine("Rocksmith tone slots after name binding: Base=" + (arrangement.ToneBase ?? "<none>") +
                                  ", A=" + (arrangement.ToneA ?? "<none>") +
                                  ", B=" + (arrangement.ToneB ?? "<none>"));
            }
        }

        private static bool EnvironmentFlag(string name, bool defaultValue)
        {
            var value = Environment.GetEnvironmentVariable(name);
            if (String.IsNullOrWhiteSpace(value)) return defaultValue;
            value = value.Trim();
            return !(value == "0" || value.Equals("false", StringComparison.OrdinalIgnoreCase) ||
                     value.Equals("no", StringComparison.OrdinalIgnoreCase) || value.Equals("off", StringComparison.OrdinalIgnoreCase));
        }

        private static void SerializeTemplate(string template, DLCPackageData info)
        {
            var settings = new XmlWriterSettings
            {
                CheckCharacters = true,
                Indent = true,
                Encoding = new System.Text.UTF8Encoding(false)
            };
            using (var writer = XmlWriter.Create(template, settings))
                new DataContractSerializer(typeof(DLCPackageData)).WriteObject(writer, info);
        }

        private static void RunPacker(string packer, string template, string packerOutput, string toolkitRoot, string label)
        {
            var arguments = "-b -t \"" + template + "\" -o \"" + packerOutput + "\"";
            Console.WriteLine("RSToolkit " + label + " template: " + template);
            Console.WriteLine("RSToolkit packer: " + packer);
            Console.WriteLine("RSToolkit " + label + " packer command: " + packer + " " + arguments);
            var psi = new ProcessStartInfo(packer, arguments)
            {
                WorkingDirectory = toolkitRoot,
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
                throw new InvalidOperationException("RSToolkit " + label + " packer.exe failed with exit code " + exitCode + ".");
        }

        public static string Generate(string destPath, DLCPackageData info, Platform platform,
            DLCPackageType dlcType = DLCPackageType.Song, int pnum = -1)
        {
            if (info == null) throw new ArgumentNullException("info");
            if (platform == null) throw new ArgumentNullException("platform");
            if ((platform.platform != GamePlatform.Pc && platform.platform != GamePlatform.Mac) ||
                platform.version != GameVersion.RS2014)
                throw new NotSupportedException("EOF PSARC export supports Rocksmith 2014 PC and Mac packages only.");
            if (dlcType != DLCPackageType.Song)
                throw new NotSupportedException("EOF PSARC export only builds song packages.");

            var toolkitRoot = AppDomain.CurrentDomain.SetupInformation.ApplicationBase;
            var packer = Path.Combine(toolkitRoot, "packer.exe");
            if (!File.Exists(packer))
                throw new FileNotFoundException("RSToolkit packer.exe was not found beside RocksmithToolkitGUI.exe.", packer);

            var outputFull = Path.GetFullPath(destPath);
            var outputDir = Path.GetDirectoryName(outputFull);
            if (String.IsNullOrEmpty(outputDir)) outputDir = Environment.CurrentDirectory;
            Directory.CreateDirectory(outputDir);

            var buildPc = EnvironmentFlag("EOF_PSARC_PC", true);
            var buildMac = EnvironmentFlag("EOF_PSARC_MAC", true);
            if (!buildPc && !buildMac)
                throw new InvalidOperationException("Select at least one PSARC platform: PC or Mac.");

            info.XBox360 = false;
            info.PS3 = false;
            info.SignatureType = PackageMagic.CON;
            NormalizeRocksmithToneSlots(info);
            NormalizeRocksmithToneNames(info);
            Console.WriteLine("PSARC targets: PC=" + buildPc + ", Mac=" + buildMac);
            Console.WriteLine("PSARC packaging mode: one independent RSToolkit packer process per selected desktop platform.");

            var stem = Path.GetFileNameWithoutExtension(outputFull);
            if (stem.EndsWith("_p", StringComparison.OrdinalIgnoreCase) ||
                stem.EndsWith("_m", StringComparison.OrdinalIgnoreCase))
                stem = stem.Substring(0, stem.Length - 2);
            var packerOutput = Path.Combine(outputDir, stem + ".psarc");
            var pcOutput = Path.Combine(outputDir, stem + "_p.psarc");
            var macOutput = Path.Combine(outputDir, stem + "_m.psarc");

            if (buildPc)
            {
                info.Pc = true;
                info.Mac = false;
                var template = Path.Combine(outputDir, stem + ".pc.dlc.xml");
                SerializeTemplate(template, info);
                RunPacker(packer, template, packerOutput, toolkitRoot, "PC");
                if (!File.Exists(pcOutput))
                    throw new FileNotFoundException("RSToolkit PC build completed, but the PC PSARC was not generated.", pcOutput);
                Console.WriteLine("PC PSARC: " + pcOutput);
            }

            if (buildMac)
            {
                info.Pc = false;
                info.Mac = true;
                var template = Path.Combine(outputDir, stem + ".mac.dlc.xml");
                SerializeTemplate(template, info);
                RunPacker(packer, template, packerOutput, toolkitRoot, "Mac");
                if (!File.Exists(macOutput))
                    throw new FileNotFoundException("RSToolkit Mac build completed, but the Mac PSARC was not generated.", macOutput);
                Console.WriteLine("Mac PSARC: " + macOutput);
            }

            info.Pc = buildPc;
            info.Mac = buildMac;
            return buildPc ? pcOutput : macOutput;
        }
    }
}
