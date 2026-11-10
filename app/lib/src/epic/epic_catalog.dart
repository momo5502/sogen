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
  Future<List<StoreGame>> games() => _library.games();

  @override
  StoreGame? restore(Object? json) => EpicGame.fromJson(json);

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) async =>
      (await _planner.plan(game as EpicGame), '');
}
