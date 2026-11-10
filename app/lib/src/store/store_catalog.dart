import '../gog/transfer_plan.dart';
import 'store_game.dart';

/// Asks the user which of [languages] to download a game in; completes with null when they cancelled.
typedef LanguageChooser = Future<String?> Function(List<String> languages);

/// The games an account owns in a store and how to plan their installs.
abstract interface class StoreCatalog {
  Future<List<StoreGame>> games();

  /// Reads a game of this catalog that [StoreGame.toJson] wrote, or null for anything else.
  StoreGame? restore(Object? json);

  /// Plans installing [game], asking [chooseLanguage] where the store offers languages. Completes with null when the
  /// user cancelled.
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage);
}
