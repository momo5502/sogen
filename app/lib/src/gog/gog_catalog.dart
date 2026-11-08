import '../store/store_catalog.dart';
import '../store/store_game.dart';
import 'gog_account.dart';
import 'gog_cdn.dart';
import 'gog_library.dart';
import 'gog_planner.dart';
import 'transfer_plan.dart';

class GogCatalog implements StoreCatalog {
  GogCatalog(GogAccount account, GogContent content) : _library = GogLibrary(account), _planner = GogPlanner(content);

  final GogLibrary _library;
  final GogPlanner _planner;

  @override
  Future<List<StoreGame>> games() => _library.games();

  @override
  Future<(PlannedInstall, String)?> plan(StoreGame game, LanguageChooser chooseLanguage) async {
    final release = await _planner.release(game.productId);
    final language = await chooseLanguage(release.languages);
    if (language == null) {
      return null;
    }
    return (await _planner.plan(release, language), language);
  }
}
