function toggle (elt)
{
	if (elt == null)
		return;

	var eltLink = firstElement (elt);
	if (eltLink != null && eltLink.className == 't')	// toggle
	{
		var ich = elt.className.indexOf ('_');
		if (ich < 0)
		{
			eltLink.src = 'cm/tp.gif';
			elt.className += '_';
		}
		else
		{
			eltLink.src = 'cm/tm.gif';
			elt.className = elt.className.slice (0, ich);
		}
	}
}

function setView (elt, fView)
{
	var eltLink = firstElement (elt);
	if (eltLink != null && eltLink.className == 't')	// toggle
	{
		var ich = elt.className.indexOf ('_');
		if (ich < 0 && !fView)
		{
			eltLink.src = 'cm/tp.gif';
			elt.className += '_';
		}
		else if (ich >= 0 && fView)
		{
			eltLink.src = 'cm/tm.gif';
			elt.className = elt.className.slice (0, ich);
		}
	}
}

function firstElement (elt)
{
	var c = elt.firstChild;
	while (c != null) {
		if (c.nodeType == 1) // Node.ELEMENT_NODE (IE6 does not recognize it)
			return c;
		c = c.nextSibling;
	}
	return null;
}

function trimSrc (strSrc)
{
	return strSrc.slice (strSrc.lastIndexOf ('/') + 1, strSrc.lastIndexOf ('.'));
}

function getChildrenByTagName (elt, strTag)
{
	strTag = strTag.toLowerCase ();
	var rgChildren = new Array ();
	var eltChild = firstElement (elt);
	while (eltChild)
	{
		if (eltChild.tagName && eltChild.tagName.toLowerCase () == strTag)
			rgChildren.push (eltChild);
		eltChild = eltChild.nextSibling;
	}
	return rgChildren;
}

function viewAll (elt, dictTypes, attrFilters)
{
	var fView = false;

	var rgImages = getChildrenByTagName (elt, 'IMG');
	var cImages = rgImages.length;
	for (var iImage = 0; iImage < cImages; iImage++)
	{
		var strImage = trimSrc (rgImages [iImage].src);
		if (dictTypes [strImage])
		{
			fView = true;
			break;
		}
	}
	var rgElts = getChildrenByTagName (elt, 'DIV');
	var cElts = rgElts.length;
	if (cElts != 0)
	{
		var iElt;
		for (iElt = 0; iElt < cElts; iElt ++)
			fView |= viewAll (rgElts [iElt], dictTypes, attrFilters);
	}

	// ... except for those attributes that are being filtered out.
	var rgSpans = getChildrenByTagName (elt, 'SPAN');
	var cSpans = rgSpans.length;
	for (var iSpan = 0; iSpan < cSpans; iSpan++)
	{
		var strSpan = rgSpans [iSpan].firstChild.nodeValue;
		for (strzzz in attrFilters)
			if (strSpan == strzzz)
				fView = false;
	}

	elt.style.display = fView ? '' : 'none';
	return fView;
}

function getView (elt)
{
	var eltLink = firstElement (elt);
	if (eltLink != null && eltLink.className == 't')	// toggle
	{
		var ich = elt.className.indexOf ('_');
		if (ich < 0)
			return true;
	}
	return false;
}

function getParentDiv (elt)
{
	if (elt)
	{
		do
		{
			elt = elt.parentNode;
		}
		while (elt && elt.tagName != 'DIV');
	}

	return elt;
}

function getName (elt)
{
	var rgSpans = getChildrenByTagName (elt, 'SPAN');
	for (var iSpan = 0; iSpan < rgSpans.length; iSpan ++)
	{
		var span = rgSpans [iSpan];
		if (span.className == 'l')	// label
		{
			if (span.innerText)
				return span.innerText;
			else
				return elt.firstChild.nodeValue;
		}
	}
	return null;
}

function clickHandler (evt)
{
	var elt;
	if (document.layers)
		elt = evt.taget;
	else if (window.event && window.event.srcElement)
	{
		elt = window.event.srcElement;
		evt = window.event;
	}
	else if (evt && evt.stopPropagation)
		elt = evt.target;
	
	if (!elt.className && elt.parentNode)
		elt = elt.parentNode;

	if (elt.className == 'l')	// label
	{
		var strClass;
		var strField;
		var strNamespace;
		var strAssembly;

		elt = getParentDiv (elt);
		var strEltClass = elt.className;
		if (strEltClass.charAt (strEltClass.length - 1) == '_')
			strEltClass = strEltClass.slice (0, strEltClass.length - 1);

		if (strEltClass == 'x')	// constructor
		{
			strField = 'ctor';
			elt = getParentDiv (elt);
		}
		else
		if (strEltClass == 'm' ||	// method
			strEltClass == 'p' ||	// property
			strEltClass == 'e' ||	// event
			strEltClass == 'f')	// field
		{
			strField = getName (elt).toLowerCase ();
			var match = strField.match ( /[\.A-Z0-9_]*/i );
			if (match)
				strField = match [0];
			elt = getParentDiv (elt);

		}

		var strEltClass = elt.className;
		if (strEltClass.charAt (strEltClass.length - 1) == '_')
			strEltClass = strEltClass.slice (0, strEltClass.length - 1);

		if (strEltClass == 'c' ||	// class
			strEltClass == 's' ||	// struct
			strEltClass == 'i' ||	// struct
			strEltClass == 'd' ||	// delegate
			strEltClass == 'en')	// enum
		{
			strClass = getName (elt);
			if (strEltClass == 'en')
				strField = null;
			elt = getParentDiv (elt);
		}

		var strEltClass = elt.className;
		if (strEltClass.charAt (strEltClass.length - 1) == '_')
			strEltClass = strEltClass.slice (0, strEltClass.length - 1);

		if (strEltClass == 'n')
		{
			strNamespace = getName (elt);
			elt = getParentDiv (elt);
		}

		var strEltClass = elt.className;
		if (strEltClass.charAt (strEltClass.length - 1) == '_')
			strEltClass = strEltClass.slice (0, strEltClass.length - 1);

		if (strEltClass == 'y')
		{
			strAssembly = getName (elt);
		}

		if (evt.ctrlKey)
		{
			var strRoot = 'http://cvs.hispalinux.es/cgi-bin/cvsweb/mcs/class/';
			var strExtra = '?cvsroot=mono';

			if (strAssembly)
			{
				if (strAssembly == 'mscorlib')
					strAssembly = 'corlib';

				strRoot = strRoot + strAssembly + '/';
				if (strNamespace)
				{
					strRoot = strRoot + strNamespace + '/';
					if (strClass)
					{
						strRoot += strClass + '.cs';
						strExtra += '&rev=1';
					}
				}
				window.open (strRoot + strExtra, 'CVS');
			}
		}
		else if (strNamespace)
		{
			if (strClass)
				strNamespace += '.' + strClass.toLowerCase ();
			if (strField)
				strNamespace += '.' + strField;
			window.open ('http://msdn2.microsoft.com/library/' + strNamespace + '.aspx', 'MSDN');
		}
	}
	else
	{
		if (elt.parentNode && elt.parentNode.className == 't')	// toggle
			elt = elt.parentNode;
		else if (elt.className != 't')	// toggle
			return;

		while (elt != null && elt.tagName != 'DIV')
			elt = elt.parentNode;
		
		if (evt.shiftKey)
		{
			var rgElts = getChildrenByTagName (elt, 'DIV');
			var cElts = rgElts.length;
			if (cElts != 0)
			{
				var fView = false;
				var iElt;
				for (iElt = 0; iElt < cElts; iElt ++)
				{
					if (getView (rgElts [iElt]))
					{
						fView = true;
						break;
					}
				}
				for (iElt = 0; iElt < cElts; iElt ++)
				{
					setView (rgElts [iElt], !fView);
				}
			}
		}
		else if (evt.ctrlKey)
		{
			setView (elt, true);
			var eltParent = getParentDiv (elt);
			while (eltParent)
			{
				var rgSiblings = getChildrenByTagName (eltParent, 'DIV');
				var cSiblings = rgSiblings.length;
				for (var iSibling = 0; iSibling < cSiblings; iSibling++)
				{
					var eltSibling = rgSiblings [iSibling];
					if (eltSibling != elt)
					{
						setView (eltSibling, false);
					}
				}
				elt = eltParent;
				eltParent = getParentDiv (elt);
			}
		}
		else
			toggle (elt);
	}

	return false;
}

function filterTree ()
{
	var eltMissing = document.getElementById ('missing');
	var eltTodo = document.getElementById ('todo');
	var eltExtra = document.getElementById ('extra');
	var eltErrors = document.getElementById ('errors');
	var eltComVisible = document.getElementById ('ComVisible');
	var eltDebuggerDisplay = document.getElementById ('DebuggerDisplay');

	var dictTypes = new Object ();
	if (eltMissing.checked)
		dictTypes ['sm'] = true;
	if (eltTodo.checked)
		dictTypes ['st'] = true;
	if (eltErrors.checked)
		dictTypes ['se'] = true;
	if (eltExtra.checked)
		dictTypes ['sx'] = true;
//	dictTypes ['sc'] = true;

	var attrFilters = new Object ();
	if (!eltComVisible.checked)
		attrFilters ['System.Runtime.InteropServices.ComVisibleAttribute'] = true;
	if (!eltDebuggerDisplay.checked)
		attrFilters ['System.Diagnostics.DebuggerDisplayAttribute'] = true;
	viewAll (document.getElementById ('ROOT'), dictTypes, attrFilters);
}

function selectMissing ()
{
	toggleFilter ('missing');
}

function selectTodo ()
{
	toggleFilter ('todo');
}

function selectExtra ()
{
	toggleFilter ('extra');
}

function selectErrors ()
{
	toggleFilter ('errors');
}

function toggleAttributeFilter (attrName)
{
	toggleFilter (attrName);
}

function toggleFilter (strFilter)
{
	var eltTodo = document.getElementById ('todo');
	var eltMissing = document.getElementById ('missing');
	var eltExtra = document.getElementById ('extra');
	var eltErrors = document.getElementById ('errors');

	var eltToggle = document.getElementById (strFilter);
	if (window && window.event && window.event.shiftKey)
	{
		eltMissing.checked = eltTodo.checked = eltExtra.checked = eltErrors.checked = false;
		eltToggle.checked = true;
	}
	else
	if (!eltTodo.checked && !eltMissing.checked && !eltExtra.checked && !eltErrors.checked)
	{
		eltMissing.checked = eltTodo.checked = eltExtra.checked = eltErrors.checked = true;
		eltToggle.checked = false;
	}
	filterTree ();
}

function onLoad ()
{
	var eltMissing = document.getElementById ('missing');
	var eltTodo = document.getElementById ('todo');
	var eltExtra = document.getElementById ('extra');
	var eltErrors = document.getElementById ('errors');
	eltMissing.checked = eltTodo.checked = eltExtra.checked = eltErrors.checked = true;
}

if (document.layers)
{
	document.captureEvents (Event.MOUSEUP);
	document.onmouseup = clickHandler;
}
else if (document.attachEvent)
{
	document.attachEvent('onclick', clickHandler);
}
else if (document.addEventListener)
{
	document.addEventListener('click', clickHandler, false);
}
else 
	document.onclick = clickHandler;

