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

  test('a script without a registry section sets nothing', () {
    expect(installScriptRegistry('"InstallScript"\n{\n}\n', r'C:\game'), isNull);
  });
}
