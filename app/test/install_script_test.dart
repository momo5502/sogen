import 'package:flutter_test/flutter_test.dart';
import 'package:sogen_app/src/steam/install_script.dart';

void main() {
  test('converts the registry section into a .reg file in the Steam client\'s 32-bit view', () {
    const script = r'''
"InstallScript"
{
	"Registry"
	{
		"HKEY_LOCAL_MACHINE\\Software\\Example Studio\\Example Game"
		{
			"string"
			{
				"Installed Path"		"%INSTALLDIR%\\"
				"Quote"		"a \"b\""
			}
			"dword"
			{
				"Version"		"258"
			}
		}
		"HKEY_CURRENT_USER\\Software\\Example Game"
		{
			"string"
			{
				"Language"		"english"
			}
		}
	}
	"Run Process"
	{
		"setup"
		{
			"process 1"		"%INSTALLDIR%\\setup.exe"
		}
	}
}
''';

    expect(
      installScriptRegistry(script, r'C:\game'),
      [
        'Windows Registry Editor Version 5.00',
        '',
        r'[hkey_local_machine\software\WOW6432Node\example studio\example game]',
        r'"installed path"="C:\\game\\"',
        r'"quote"="a \"b\""',
        '"version"=dword:00000102',
        '',
        r'[hkey_current_user\software\example game]',
        '"language"="english"',
        '',
      ].join('\r\n'),
    );
  });

  test('takes the values of the game\'s language, else English, and writes (Default) as the unnamed value', () {
    const script = r'''
"InstallScript"
{
	"Registry"
	{
		"HKEY_CURRENT_USER\\Software\\Example Game"
		{
			"string"
			{
				"(Default)"		"example"
				"english"
				{
					"Name"		"Example"
				}
				"german"
				{
					"Name"		"Beispiel"
				}
			}
		}
	}
}
''';

    String registryIn(String language) => installScriptRegistry(script, r'C:\game', language: language)!;

    expect(registryIn('german'), contains('"name"="Beispiel"'));
    expect(registryIn('german'), isNot(contains('"name"="Example"')));
    expect(registryIn('french'), contains('"name"="Example"'));
    expect(registryIn('german'), contains('@="example"'));
    expect(registryIn('german'), isNot(contains('(default)')));
  });

  test('resolves Steam\'s folder names to the install folder or to guest environment variables', () {
    const script = r'''
"InstallScript"
{
	"Registry"
	{
		"HKEY_CURRENT_USER\\Software\\Example Game"
		{
			"string"
			{
				"Drive"		"%ROOTDRIVE%\\"
				"Saves"		"%user_mydocs%\\My Games\\Example"
				"Shared"		"%COMMON_MYDOCS%\\Example"
				"Cache"		"%LOCALAPPDATA%\\Example"
				"Other"		"%TEMP%\\Example"
			}
		}
	}
}
''';

    final registry = installScriptRegistry(script, r'D:\game')!;

    expect(registry, contains(r'"drive"="D:\\"'));
    expect(registry, contains(r'"saves"="%USERPROFILE%\\Documents\\My Games\\Example"'));
    expect(registry, contains(r'"shared"="%SystemDrive%\\Users\\Public\\Documents\\Example"'));
    expect(registry, contains(r'"cache"="%USERPROFILE%\\AppData\\Local\\Example"'));
    expect(registry, contains(r'"other"="%TEMP%\\Example"'));
  });

  test('a script without a registry section sets nothing', () {
    expect(installScriptRegistry('"InstallScript"\n{\n}\n', r'C:\game'), isNull);
  });
}
