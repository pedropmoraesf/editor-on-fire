using System;
using System.IO;
using System.Text;
using Newtonsoft.Json.Linq;

namespace EofPsarcHelper
{
    /*
     * Rocksmith preview selector shared by the PSARC build path.
     * It intentionally mirrors EOF's DTX preview policy: scan 25-second PCM
     * windows every five seconds, choose the first window with the highest RMS
     * energy and crop it without fades.  The PSARC count-in prefix is excluded
     * from the analysis so the preview is chosen from the music itself.
     */
    internal static class HotPreview
    {
        private const long PreviewMilliseconds = 25000L;
        private const long StepMilliseconds = 5000L;

        private static double WindowEnergy(FileStream input, long absoluteOffset, long byteCount)
        {
            var buffer = new byte[65536];
            long remaining = byteCount;
            double energy = 0.0;
            long samples = 0L;

            input.Position = absoluteOffset;
            while (remaining > 0L)
            {
                var request = (int)Math.Min((long)buffer.Length, remaining);
                if ((request & 1) != 0) request--;
                if (request <= 0) break;
                var got = input.Read(buffer, 0, request);
                if (got <= 0) break;
                if ((got & 1) != 0) got--;

                for (var i = 0; i + 1 < got; i += 2)
                {
                    var sample = (short)(buffer[i] | (buffer[i + 1] << 8));
                    var value = (double)sample;
                    energy += value * value;
                    samples++;
                }
                remaining -= got;
            }

            if (remaining != 0L || samples == 0L)
                return -1.0;
            return energy / samples;
        }

        internal static void Create(JObject spec, string source, string dest)
        {
            var prefixMs = (long?)spec["prefixMs"] ?? 0L;
            var musicLengthMs = (long?)spec["musicLengthMs"] ?? 0L;

            try
            {
                using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var br = new BinaryReader(input, Encoding.ASCII))
                {
                    if (Encoding.ASCII.GetString(br.ReadBytes(4)) != "RIFF")
                        throw new InvalidDataException("Not RIFF");
                    br.ReadUInt32();
                    if (Encoding.ASCII.GetString(br.ReadBytes(4)) != "WAVE")
                        throw new InvalidDataException("Not WAVE");

                    ushort format = 0, channels = 0, blockAlign = 0, bits = 0;
                    uint rate = 0, byteRate = 0;
                    long dataOffset = -1L, dataBytes = 0L;

                    while (input.Position + 8L <= input.Length)
                    {
                        var id = Encoding.ASCII.GetString(br.ReadBytes(4));
                        var size = br.ReadUInt32();
                        var payload = input.Position;
                        var available = Math.Max(0L, Math.Min((long)size, input.Length - payload));

                        if (id == "fmt " && available >= 16L)
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
                        if (dataOffset >= 0L && format != 0) break;
                    }

                    if (format != 1 || channels == 0 || rate == 0 || byteRate == 0 || blockAlign == 0 || bits != 16 || dataOffset < 0L || dataBytes <= 0L)
                        throw new InvalidDataException("Unsupported staged WAV format for RMS preview scan");

                    var musicStartByte = (prefixMs * (long)byteRate) / 1000L;
                    musicStartByte -= musicStartByte % blockAlign;
                    if (musicStartByte < 0L) musicStartByte = 0L;
                    if (musicStartByte >= dataBytes) musicStartByte = 0L;

                    var availableMusicBytes = dataBytes - musicStartByte;
                    var declaredMusicBytes = musicLengthMs > 0L ? (musicLengthMs * (long)byteRate) / 1000L : availableMusicBytes;
                    declaredMusicBytes -= declaredMusicBytes % blockAlign;
                    var musicBytes = Math.Min(availableMusicBytes, declaredMusicBytes > 0L ? declaredMusicBytes : availableMusicBytes);
                    musicBytes -= musicBytes % blockAlign;
                    if (musicBytes <= 0L)
                        throw new InvalidDataException("Empty music range for preview");

                    var windowBytes = (PreviewMilliseconds * (long)byteRate) / 1000L;
                    windowBytes -= windowBytes % blockAlign;
                    var stepBytes = (StepMilliseconds * (long)byteRate) / 1000L;
                    stepBytes -= stepBytes % blockAlign;
                    if (windowBytes <= 0L || stepBytes <= 0L)
                        throw new InvalidDataException("Invalid preview scan window");

                    long bestOffset = 0L;
                    double bestEnergy = -1.0;
                    long previewBytes;

                    if (musicBytes <= windowBytes)
                    {
                        previewBytes = musicBytes;
                    }
                    else
                    {
                        previewBytes = windowBytes;
                        for (long offset = 0L; offset < musicBytes - windowBytes; offset += stepBytes)
                        {
                            var energy = WindowEnergy(input, dataOffset + musicStartByte + offset, windowBytes);
                            if (energy > bestEnergy)
                            {
                                bestEnergy = energy;
                                bestOffset = offset;
                            }
                        }
                        if (bestEnergy < 0.0)
                            throw new InvalidDataException("Could not measure preview RMS windows");
                    }

                    var copyBytes = Math.Min(previewBytes, musicBytes - bestOffset);
                    copyBytes -= copyBytes % blockAlign;
                    if (copyBytes <= 0L)
                        throw new InvalidDataException("Empty hottest preview range");

                    input.Position = dataOffset + musicStartByte + bestOffset;
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
                        while (left > 0L)
                        {
                            var request = (int)Math.Min((long)buffer.Length, left);
                            var got = input.Read(buffer, 0, request);
                            if (got <= 0) throw new EndOfStreamException();
                            output.Write(buffer, 0, got);
                            left -= got;
                        }
                    }

                    var startMs = prefixMs + ((bestOffset * 1000L) / byteRate);
                    var lengthMs = (copyBytes * 1000L) / byteRate;
                    Console.WriteLine("PSARC preview: DTX hottest 25s RMS window, no fades; start " + startMs + " ms, length " + lengthMs + " ms.");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("PSARC hottest preview fallback: " + ex.Message);
                File.Copy(source, dest, true);
            }
        }
    }
}
