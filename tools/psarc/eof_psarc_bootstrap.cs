using System;
using System.IO;
using System.Reflection;

namespace EofPsarcBootstrap
{
    internal static class Program
    {
        private static string dependencyRoot;

        private static Assembly ResolveBundledAssembly(object sender, ResolveEventArgs args)
        {
            try
            {
                var simpleName = new AssemblyName(args.Name).Name + ".dll";
                var candidate = Path.Combine(dependencyRoot, simpleName);
                if (File.Exists(candidate))
                    return Assembly.LoadFrom(candidate);
            }
            catch
            {
            }
            return null;
        }

        private static void LoadBundledAssemblies(string root)
        {
            /* Load dependencies explicitly before the worker.  This makes the
             * EOF/bin copies win even if the user's RSToolkit installation has
             * another version of the same assemblies beside the helper. */
            var names = new[]
            {
                "ICSharpCode.SharpZipLib.dll",
                "MiscUtil.dll",
                "Newtonsoft.Json.dll",
                "X360.dll",
                "zlib.net.dll",
                "RocksmithToolkitLib.dll"
            };

            foreach (var name in names)
            {
                var path = Path.Combine(root, name);
                if (!File.Exists(path))
                    throw new FileNotFoundException("Bundled PSARC dependency was not found: " + path);
                Assembly.LoadFrom(path);
            }
        }

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length < 1)
                    throw new ArgumentException("Usage: eof_psarc_helper.exe <spec.json>");

                var appRoot = AppDomain.CurrentDomain.SetupInformation.ApplicationBase;
                var dependencyFile = Path.Combine(appRoot, "eof_psarc_dependencies.txt");
                if (!File.Exists(dependencyFile))
                    throw new FileNotFoundException("EOF PSARC dependency-path file was not found: " + dependencyFile);

                dependencyRoot = File.ReadAllText(dependencyFile).Trim();
                if (String.IsNullOrEmpty(dependencyRoot) || !Directory.Exists(dependencyRoot))
                    throw new DirectoryNotFoundException("EOF bundled PSARC dependency directory was not found: " + dependencyRoot);

                AppDomain.CurrentDomain.AssemblyResolve += ResolveBundledAssembly;
                LoadBundledAssemblies(dependencyRoot);

                var workerPath = Path.Combine(appRoot, "eof_psarc_worker.dll");
                if (!File.Exists(workerPath))
                    throw new FileNotFoundException("EOF PSARC worker was not found: " + workerPath);

                var worker = Assembly.LoadFrom(workerPath);
                var type = worker.GetType("EofPsarcHelper.Program", true);
                var method = type.GetMethod("Main", BindingFlags.Static | BindingFlags.NonPublic);
                if (method == null)
                    throw new MissingMethodException("EofPsarcHelper.Program.Main was not found.");

                try
                {
                    var result = method.Invoke(null, new object[] { new[] { args[0] } });
                    return (result is int) ? (int)result : 0;
                }
                catch (TargetInvocationException ex)
                {
                    throw ex.InnerException ?? ex;
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(ex.ToString());
                return 2;
            }
        }
    }
}
