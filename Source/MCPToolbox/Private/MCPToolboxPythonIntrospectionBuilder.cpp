#include "MCPToolboxPythonIntrospectionBuilder.h"

FString FMCPToolboxPythonIntrospectionBuilder::BuildModuleIntrospectionScript(
	const FString& FilterCondition,
	const FString& TypeFiltering,
	const FString& MaxItemsCode)
{
	return FString::Printf(TEXT(
		"import unreal\n"
		"import inspect\n"
		"import json\n"
		"\n"
		"result = {'module_name': 'unreal', 'classes': [], 'functions': [], 'constants': [], 'total_members': 0}\n"
		"\n"
		"for name, obj in inspect.getmembers(unreal):\n"
		"    if not (%s%s):\n"
		"        continue\n"
		"    result['total_members'] += 1\n"
		"    if inspect.isclass(obj):\n"
		"        result['classes'].append(name)\n"
		"    elif inspect.isfunction(obj) or inspect.isbuiltin(obj):\n"
		"        result['functions'].append(name)\n"
		"    elif not name.startswith('_'):\n"
		"        result['constants'].append(name)\n"
		"%s"
		"\n"
		"print(json.dumps(result))\n"
	), *FilterCondition, *TypeFiltering, *MaxItemsCode);
}

FString FMCPToolboxPythonIntrospectionBuilder::BuildClassIntrospectionScript(
	const FString& ClassName,
	const FString& InheritanceFilter,
	const FString& PrivacyFilter,
	const FString& MethodFilterCondition,
	const FString& MaxMethodsCode)
{
	return FString::Printf(TEXT(
		"import unreal\n"
		"import inspect\n"
		"import json\n"
		"\n"
		"try:\n"
		"    cls = getattr(unreal, '%s')\n"
		"    if not inspect.isclass(cls):\n"
		"        raise ValueError('Not a class')\n"
		"\n"
		"    result = {\n"
		"        'name': '%s',\n"
		"        'full_path': 'unreal.%s',\n"
		"        'docstring': inspect.getdoc(cls) or '',\n"
		"        'base_classes': [b.__name__ for b in inspect.getmro(cls)[1:]],\n"
		"        'methods': [],\n"
		"        'properties': [],\n"
		"        'is_abstract': inspect.isabstract(cls)\n"
		"    }\n"
		"\n"
		"    for name, obj in %s:\n"
		"%s"
		"        if not (%s):\n"
		"            continue\n"
		"        if inspect.ismethod(obj) or inspect.isfunction(obj) or inspect.isbuiltin(obj) or inspect.ismethoddescriptor(obj):\n"
		"            doc = inspect.getdoc(obj) or ''\n"
		"            try:\n"
		"                sig = str(inspect.signature(obj))\n"
		"            except:\n"
		"                sig = '(...)'\n"
		"                if doc:\n"
		"                    import re\n"
		"                    match = re.match(r'X\\.\\w+\\(([^)]*)\\)\\s*(?:->\\s*(\\S+))?', doc)\n"
		"                    if match:\n"
		"                        params = match.group(1)\n"
		"                        ret = match.group(2) or 'None'\n"
		"                        sig = f'({params}) -> {ret}'\n"
		"            result['methods'].append({\n"
		"                'name': name,\n"
		"                'signature': sig,\n"
		"                'docstring': doc\n"
		"            })\n"
		"%s"
		"        elif not callable(obj):\n"
		"            result['properties'].append(name)\n"
		"\n"
		"    print(json.dumps(result))\n"
		"except AttributeError:\n"
		"    print(json.dumps({'error': 'Class not found'}))\n"
		"except Exception as e:\n"
		"    print(json.dumps({'error': str(e)}))\n"
	), *ClassName, *ClassName, *ClassName, *InheritanceFilter,
	   *PrivacyFilter, *MethodFilterCondition, *MaxMethodsCode);
}

FString FMCPToolboxPythonIntrospectionBuilder::BuildFunctionIntrospectionScript(
	const FString& FunctionName,
	const FString& ClassName,
	bool bIsClassMethod)
{
	if (bIsClassMethod)
	{
		return FString::Printf(TEXT(
			"import unreal\n"
			"import inspect\n"
			"import json\n"
			"import re\n"
			"\n"
			"try:\n"
			"    cls = getattr(unreal, '%s')\n"
			"    if not inspect.isclass(cls):\n"
			"        raise ValueError('Not a class')\n"
			"    func = getattr(cls, '%s')\n"
			"    if func is None:\n"
			"        raise AttributeError('Method not found')\n"
			"\n"
			"    doc = inspect.getdoc(func) or ''\n"
			"    result = {\n"
			"        'name': '%s.%s',\n"
			"        'docstring': doc,\n"
			"        'is_method': True,\n"
			"        'is_static': isinstance(inspect.getattr_static(cls, '%s'), staticmethod),\n"
			"        'is_class_method': isinstance(inspect.getattr_static(cls, '%s'), classmethod)\n"
			"    }\n"
			"\n"
			"    try:\n"
			"        sig = inspect.signature(func)\n"
			"        result['signature'] = str(sig)\n"
			"        result['parameters'] = [p.name for p in sig.parameters.values()]\n"
			"        result['param_types'] = [str(p.annotation) if p.annotation != inspect.Parameter.empty else 'Any' for p in sig.parameters.values()]\n"
			"        result['return_type'] = str(sig.return_annotation) if sig.return_annotation != inspect.Signature.empty else 'Any'\n"
			"    except:\n"
			"        result['signature'] = '(...)'\n"
			"        result['parameters'] = []\n"
			"        result['param_types'] = []\n"
			"        result['return_type'] = 'Any'\n"
			"        if doc:\n"
			"            match = re.match(r'X\\.\\w+\\(([^)]*)\\)\\s*(?:->\\s*(\\S+))?', doc)\n"
			"            if match:\n"
			"                params = match.group(1)\n"
			"                ret = match.group(2) or 'None'\n"
			"                result['signature'] = f'({params}) -> {ret}'\n"
			"                if params:\n"
			"                    result['parameters'] = [p.strip().split('=')[0].strip() for p in params.split(',')]\n"
			"                    result['return_type'] = ret\n"
			"\n"
			"    print(json.dumps(result))\n"
			"except AttributeError:\n"
			"    print(json.dumps({'error': 'Method not found on class'}))\n"
			"except Exception as e:\n"
			"    print(json.dumps({'error': str(e)}))\n"
		), *ClassName, *FunctionName, *ClassName, *FunctionName, *FunctionName, *FunctionName);
	}
	else
	{
		return FString::Printf(TEXT(
			"import unreal\n"
			"import inspect\n"
			"import json\n"
			"import re\n"
			"\n"
			"try:\n"
			"    func = getattr(unreal, '%s')\n"
			"    if not (inspect.isfunction(func) or inspect.isbuiltin(func)):\n"
			"        raise ValueError('Not a function')\n"
			"\n"
			"    doc = inspect.getdoc(func) or ''\n"
			"    result = {\n"
			"        'name': '%s',\n"
			"        'docstring': doc,\n"
			"        'is_method': False,\n"
			"        'is_static': False,\n"
			"        'is_class_method': False\n"
			"    }\n"
			"\n"
			"    try:\n"
			"        sig = inspect.signature(func)\n"
			"        result['signature'] = str(sig)\n"
			"        result['parameters'] = [p.name for p in sig.parameters.values()]\n"
			"        result['param_types'] = [str(p.annotation) if p.annotation != inspect.Parameter.empty else 'Any' for p in sig.parameters.values()]\n"
			"        result['return_type'] = str(sig.return_annotation) if sig.return_annotation != inspect.Signature.empty else 'Any'\n"
			"    except:\n"
			"        result['signature'] = '(...)'\n"
			"        result['parameters'] = []\n"
			"        result['param_types'] = []\n"
			"        result['return_type'] = 'Any'\n"
			"        if doc:\n"
			"            match = re.match(r'(?:X\\.)?\\w+\\(([^)]*)\\)\\s*(?:->\\s*(\\S+))?', doc)\n"
			"            if match:\n"
			"                params = match.group(1)\n"
			"                ret = match.group(2) or 'None'\n"
			"                result['signature'] = f'({params}) -> {ret}'\n"
			"                if params:\n"
			"                    result['parameters'] = [p.strip().split('=')[0].strip() for p in params.split(',')]\n"
			"                    result['return_type'] = ret\n"
			"\n"
			"    print(json.dumps(result))\n"
			"except AttributeError:\n"
			"    print(json.dumps({'error': 'Function not found'}))\n"
			"except Exception as e:\n"
			"    print(json.dumps({'error': str(e)}))\n"
		), *FunctionName, *FunctionName);
	}
}

FString FMCPToolboxPythonIntrospectionBuilder::BuildSubsystemListScript()
{
	return TEXT(
		"import unreal\n"
		"import inspect\n"
		"import json\n"
		"\n"
		"result = {'subsystems': []}\n"
		"\n"
		"for name, obj in inspect.getmembers(unreal):\n"
		"    if inspect.isclass(obj) and 'Subsystem' in name and 'Editor' in name:\n"
		"        result['subsystems'].append(name)\n"
		"\n"
		"print(json.dumps(result))\n"
	);
}

