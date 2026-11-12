import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';
import 'package:url_launcher/url_launcher.dart';

final Uri discordUrl = Uri.parse('https://discord.sogen.dev');

Future<void> openDiscord(BuildContext context) async {
  final messenger = ScaffoldMessenger.of(context);
  bool opened;
  try {
    opened = await launchUrl(discordUrl, mode: LaunchMode.externalApplication);
  } on PlatformException {
    opened = false;
  }

  if (!opened) {
    messenger.showSnackBar(SnackBar(content: Text('Could not open $discordUrl')));
  }
}
