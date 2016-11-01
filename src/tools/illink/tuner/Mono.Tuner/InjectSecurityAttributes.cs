//
// InjectSecurityAttributes.cs
//
// Author:
//   Jb Evain (jbevain@novell.com)
//
// (C) 2009 Novell, Inc.
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

using System;
using System.Collections;
using System.IO;

using Mono.Linker;
using Mono.Linker.Steps;

using Mono.Cecil;

namespace Mono.Tuner {

	public class InjectSecurityAttributes : BaseStep {

		enum TargetKind {
			Type,
			Method,
		}

		enum AttributeType {
			Critical,
			SafeCritical,
		}

		const string _safe_critical = "System.Security.SecuritySafeCriticalAttribute";
		const string _critical = "System.Security.SecurityCriticalAttribute";

		const string sec_attr_folder = "secattrs";

		AssemblyDefinition _assembly;

		MethodDefinition _safe_critical_ctor;
		MethodDefinition _critical_ctor;

		string data_folder;

		protected override bool ConditionToProcess ()
		{
			if (!Context.HasParameter (sec_attr_folder))
				return false;

			data_folder = Context.GetParameter (sec_attr_folder);
			return true;
		}

		protected override void ProcessAssembly (AssemblyDefinition assembly)
		{
			if (Annotations.GetAction (assembly) != AssemblyAction.Link)
				return;

			string secattr_file = Path.Combine (
				data_folder,
				assembly.Name.Name + ".secattr");

			if (!File.Exists (secattr_file))
				return;

			_assembly = assembly;

			// remove existing [SecurityCritical] and [SecuritySafeCritical]
			RemoveSecurityAttributes ();

			// add [SecurityCritical] and [SecuritySafeCritical] from the data file
			ProcessSecurityAttributeFile (secattr_file);
		}

		void RemoveSecurityAttributes ()
		{
			foreach (TypeDefinition type in _assembly.MainModule.Types) {
				RemoveSecurityAttributes (type);

				if (type.HasConstructors)
					foreach (MethodDefinition ctor in type.Constructors)
						RemoveSecurityAttributes (ctor);

				if (type.HasMethods)
					foreach (MethodDefinition method in type.Methods)
						RemoveSecurityAttributes (method);
			}
		}

		static void RemoveSecurityAttributes (ICustomAttributeProvider provider)
		{
			if (!provider.HasCustomAttributes)
				return;

			CustomAttributeCollection attributes = provider.CustomAttributes;
			for (int i = 0; i < attributes.Count; i++) {
				CustomAttribute attribute = attributes [i];
				switch (attribute.Constructor.DeclaringType.FullName) {
				case _safe_critical:
				case _critical:
					attributes.RemoveAt (i--);
					break;
				}
			}
		}

		void ProcessSecurityAttributeFile (string file)
		{
			using (StreamReader reader = File.OpenText (file)) {
				string line;
				while ((line = reader.ReadLine ()) != null)
					ProcessLine (line);
			}
		}

		void ProcessLine (string line)
		{
			if (line == null || line.Length < 6)
				return;

			int sep = line.IndexOf (": ");
			if (sep == -1)
				return;

			string marker = line.Substring (0, sep);
			string target = line.Substring (sep + 2);

			ProcessSecurityAttributeEntry (
				DecomposeAttributeType (marker),
				DecomposeTargetKind (marker),
				target);
		}

		static AttributeType DecomposeAttributeType (string marker)
		{
			if (marker.StartsWith ("SC"))
				return AttributeType.Critical;
			else if (marker.StartsWith ("SSC"))
				return AttributeType.SafeCritical;
			else
				throw new ArgumentException ();
		}

		static TargetKind DecomposeTargetKind (string marker)
		{
			switch (marker [marker.Length - 1]) {
			case 'T':
				return TargetKind.Type;
			case 'M':
				return TargetKind.Method;
			default:
				throw new ArgumentException ();
			}
		}

		void ProcessSecurityAttributeEntry (AttributeType type, TargetKind kind, string target)
		{
			ICustomAttributeProvider provider = GetTarget (kind, target);
			if (provider == null)
				return;

			switch (type) {
			case AttributeType.Critical:
				AddCriticalAttribute (provider);
				break;
			case AttributeType.SafeCritical:
				AddSafeCriticalAttribute (provider);
				break;
			}
		}

		void AddCriticalAttribute (ICustomAttributeProvider provider)
		{
			// a [SecurityCritical] replaces a [SecuritySafeCritical]
			if (HasSecurityAttribute (provider, AttributeType.SafeCritical))
				RemoveSecurityAttributes (provider);

			AddSecurityAttribute (provider, AttributeType.Critical);
		}

		void AddSafeCriticalAttribute (ICustomAttributeProvider provider)
		{
			// a [SecuritySafeCritical] is ignored if a [SecurityCritical] is present
			if (HasSecurityAttribute (provider, AttributeType.Critical))
				return;

			AddSecurityAttribute (provider, AttributeType.SafeCritical);
		}

		void AddSecurityAttribute (ICustomAttributeProvider provider, AttributeType type)
		{
			if (HasSecurityAttribute (provider, type))
				return;

			CustomAttributeCollection attributes = provider.CustomAttributes;
			switch (type) {
			case AttributeType.Critical:
				attributes.Add (CreateCriticalAttribute ());
				break;
			case AttributeType.SafeCritical:
				attributes.Add (CreateSafeCriticalAttribute ());
				break;
			}
		}

		static bool HasSecurityAttribute (ICustomAttributeProvider provider, AttributeType type)
		{
			if (!provider.HasCustomAttributes)
				return false;

			foreach (CustomAttribute attribute in provider.CustomAttributes) {
				switch (attribute.Constructor.DeclaringType.Name) {
				case _critical:
					if (type == AttributeType.Critical)
						return true;

					break;
				case _safe_critical:
					if (type == AttributeType.SafeCritical)
						return true;

					break;
				}
			}

			return false;
		}

		ICustomAttributeProvider GetTarget (TargetKind kind, string target)
		{
			switch (kind) {
			case TargetKind.Type:
				return GetType (target);
			case TargetKind.Method:
				return GetMethod (target);
			default:
				throw new ArgumentException ();
			}
		}

		TypeDefinition GetType (string fullname)
		{
			return _assembly.MainModule.Types [fullname];
		}

		MethodDefinition GetMethod (string signature)
		{
			int pos = signature.IndexOf (" ");
			if (pos == -1)
				throw new ArgumentException ();

			string tmp = signature.Substring (pos + 1);

			pos = tmp.IndexOf ("::");
			if (pos == -1)
				throw new ArgumentException ();

			string type_name = tmp.Substring (0, pos);

			int parpos = tmp.IndexOf ("(");
			if (parpos == -1)
				throw new ArgumentException ();

			string method_name = tmp.Substring (pos + 2, parpos - pos - 2);

			TypeDefinition type = GetType (type_name);
			if (type == null)
				return null;

			return method_name.StartsWith (".c") ?
				GetMethod (type.Constructors, signature) :
				GetMethod (type.Methods.GetMethod (method_name), signature);
		}

		static MethodDefinition GetMethod (IEnumerable methods, string signature)
		{
			foreach (MethodDefinition method in methods)
				if (method.ToString () == signature)
					return method;

			return null;
		}

		static MethodDefinition GetDefaultConstructor (TypeDefinition type)
		{
			foreach (MethodDefinition ctor in type.Constructors)
				if (ctor.Parameters.Count == 0)
					return ctor;

			return null;
		}

		MethodDefinition GetSafeCriticalCtor ()
		{
			if (_safe_critical_ctor != null)
				return _safe_critical_ctor;

			_safe_critical_ctor = GetDefaultConstructor (Context.GetType (_safe_critical));
			return _safe_critical_ctor;
		}

		MethodDefinition GetCriticalCtor ()
		{
			if (_critical_ctor != null)
				return _critical_ctor;

			_critical_ctor = GetDefaultConstructor (Context.GetType (_critical));
			return _critical_ctor;
		}

		MethodReference Import (MethodDefinition method)
		{
			return _assembly.MainModule.Import (method);
		}

		CustomAttribute CreateSafeCriticalAttribute ()
		{
			return new CustomAttribute (Import (GetSafeCriticalCtor ()));
		}

		CustomAttribute CreateCriticalAttribute ()
		{
			return new CustomAttribute (Import (GetCriticalCtor ()));
		}
	}
}
