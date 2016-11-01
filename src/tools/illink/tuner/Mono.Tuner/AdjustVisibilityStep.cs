//
// AdjustVisibilityStep.cs
//
// Author:
//   Jb Evain (jbevain@novell.com)
//
// (C) 2007 Novell, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using System.Collections;

using Mono.Linker;
using Mono.Linker.Steps;

using Mono.Cecil;

namespace Mono.Tuner {

	public class AdjustVisibility : BaseStep {

		protected override void ProcessAssembly (AssemblyDefinition assembly)
		{
			foreach (TypeDefinition type in assembly.MainModule.Types)
				ProcessType (type);
		}

		static void ProcessType (TypeDefinition type)
		{
			if (!type.IsPublic)
				return;

			if (!IsMarkedAsPublic (type)) {
				SetInternalVisibility (type);
				return;
			}

			ProcessFields (type.Fields);
			ProcessMethods (type.Constructors);
			ProcessMethods (type.Methods);
		}

		static void SetInternalVisibility (TypeDefinition type)
		{
			type.Attributes &= ~TypeAttributes.VisibilityMask;
			if (type.DeclaringType == null)
				type.Attributes |= TypeAttributes.NotPublic;
			else
				type.Attributes |= TypeAttributes.NestedAssembly;
		}

		static void ProcessMethods (ICollection methods)
		{
			foreach (MethodDefinition method in methods)
				ProcessMethod (method);
		}

		static void ProcessMethod (MethodDefinition method)
		{
			if (!method.IsPublic)
				return;

			if (IsMarkedAsPublic (method))
				return;

			SetInternalVisibility (method);
		}

		static void SetInternalVisibility (MethodDefinition method)
		{
			method.Attributes &= ~MethodAttributes.MemberAccessMask;
			method.Attributes |= MethodAttributes.Assem;
		}

		static bool IsMarkedAsPublic (IAnnotationProvider provider)
		{
			return Annotations.IsPublic (provider);
		}

		static void ProcessFields (FieldDefinitionCollection fields)
		{
			foreach (FieldDefinition field in fields)
				ProcessField (field);
		}

		static void ProcessField (FieldDefinition field)
		{
			if (!field.IsPublic)
				return;

			if (IsMarkedAsPublic (field))
				return;

			SetInternalVisibility (field);
		}

		static void SetInternalVisibility (FieldDefinition field)
		{
			field.Attributes &= ~FieldAttributes.FieldAccessMask;
			field.Attributes |= FieldAttributes.Assembly;
		}
	}
}
