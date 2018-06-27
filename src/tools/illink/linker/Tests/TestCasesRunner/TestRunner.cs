﻿using System.Linq;
using System.Threading.Tasks;
using Mono.Cecil;
using Mono.Linker.Tests.TestCases;
using NUnit.Framework;

namespace Mono.Linker.Tests.TestCasesRunner {
	public class TestRunner {
		private readonly ObjectFactory _factory;

		public TestRunner (ObjectFactory factory)
		{
			_factory = factory;
		}

		public virtual LinkedTestCaseResult Run (TestCase testCase)
		{
			using (var fullTestCaseAssemblyDefinition = AssemblyDefinition.ReadAssembly (testCase.OriginalTestCaseAssemblyPath.ToString ())) {
				var metadataProvider = _factory.CreateMetadataProvider (testCase, fullTestCaseAssemblyDefinition);

				string ignoreReason;
				if (metadataProvider.IsIgnored (out ignoreReason))
					Assert.Ignore (ignoreReason);

				var sandbox = Sandbox (testCase, metadataProvider);
				var compilationResult = Compile (sandbox, metadataProvider);
				PrepForLink (sandbox, compilationResult);
				return Link (testCase, sandbox, compilationResult, metadataProvider);
			}
		}

		private TestCaseSandbox Sandbox (TestCase testCase, TestCaseMetadaProvider metadataProvider)
		{
			var sandbox = _factory.CreateSandbox (testCase);
			sandbox.Populate (metadataProvider);
			return sandbox;
		}

		private ManagedCompilationResult Compile (TestCaseSandbox sandbox, TestCaseMetadaProvider metadataProvider)
		{
			var inputCompiler = _factory.CreateCompiler (sandbox, metadataProvider);
			var expectationsCompiler = _factory.CreateCompiler (sandbox, metadataProvider);
			var sourceFiles = sandbox.SourceFiles.Select(s => s.ToString()).ToArray();

			var assemblyName = metadataProvider.GetAssemblyName ();

			var commonReferences = metadataProvider.GetCommonReferencedAssemblies(sandbox.InputDirectory).ToArray ();
			var mainAssemblyReferences = metadataProvider.GetReferencedAssemblies(sandbox.InputDirectory).ToArray ();
			var resources = sandbox.ResourceFiles.ToArray ();
			var additionalArguments = metadataProvider.GetSetupCompilerArguments ().ToArray ();
			
			var expectationsCommonReferences = metadataProvider.GetCommonReferencedAssemblies (sandbox.ExpectationsDirectory).ToArray ();
			var expectationsMainAssemblyReferences = metadataProvider.GetReferencedAssemblies (sandbox.ExpectationsDirectory).ToArray ();

			var inputTask = Task.Run(() => inputCompiler.CompileTestIn (sandbox.InputDirectory, assemblyName, sourceFiles, commonReferences, mainAssemblyReferences, null, resources, additionalArguments));
			var expectationsTask = Task.Run(() => expectationsCompiler.CompileTestIn (sandbox.ExpectationsDirectory, assemblyName, sourceFiles, expectationsCommonReferences, expectationsMainAssemblyReferences, new[] {"INCLUDE_EXPECTATIONS"}, resources, additionalArguments));

			var inputAssemblyPath = inputTask.Result;
			var expectationsAssemblyPath = expectationsTask.Result;
			return new ManagedCompilationResult (inputAssemblyPath, expectationsAssemblyPath);
		}

		protected virtual void PrepForLink (TestCaseSandbox sandbox, ManagedCompilationResult compilationResult)
		{
		}

		private LinkedTestCaseResult Link (TestCase testCase, TestCaseSandbox sandbox, ManagedCompilationResult compilationResult, TestCaseMetadaProvider metadataProvider)
		{
			var linker = _factory.CreateLinker ();
			var builder = _factory.CreateLinkerArgumentBuilder ();
			var caseDefinedOptions = metadataProvider.GetLinkerOptions ();

			builder.AddOutputDirectory (sandbox.OutputDirectory);
			foreach (var linkXmlFile in sandbox.LinkXmlFiles)
				builder.AddLinkXmlFile (linkXmlFile);

			builder.AddSearchDirectory (sandbox.InputDirectory);
			foreach (var extraSearchDir in metadataProvider.GetExtraLinkerSearchDirectories ())
				builder.AddSearchDirectory (extraSearchDir);

			builder.ProcessOptions (caseDefinedOptions);

			AddAdditionalLinkOptions (builder, metadataProvider);

			// TODO: Should be overridable
			builder.LinkFromAssembly (compilationResult.InputAssemblyPath.ToString ());

			linker.Link (builder.ToArgs ());

			return new LinkedTestCaseResult (testCase, compilationResult.InputAssemblyPath, sandbox.OutputDirectory.Combine (compilationResult.InputAssemblyPath.FileName), compilationResult.ExpectationsAssemblyPath);
		}

		protected virtual void AddAdditionalLinkOptions (LinkerArgumentBuilder builder, TestCaseMetadaProvider metadataProvider)
		{
		}
	}
}