import '../gog/transfer_plan.dart';
import '../store/store_catalog.dart';
import '../store/store_game.dart';
import 'epic_library.dart';
import 'epic_planner.dart';

/// Epic builds carry every language, so an Epic game installs without choosing one.
class EpicCatalog implements StoreCatalog {
  EpicCatalog(EpicApi api, EpicContent content) : _library = EpicLibrary(api), _planner = EpicPlanner(content);

  final EpicLibrary _library;
  final EpicPlanner _planner;

  @override
  String get name => 'Epic';

  @override
  String get signInDescription =>
      "Sign in to see the Windows games you own on Epic. You sign in on Epic's own page, so Sogen never sees your password.";

  @override
  Future<List<StoreGame>> games() => _library.games();

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) async =>
      (await _planner.plan(game as EpicGame), '');
}
