/// A game the user owns in a store. [productId] names the product in that store and stays the same across its
/// builds and languages, which are chosen when installing.
class StoreGame {
  const StoreGame({required this.store, required this.productId, required this.title, this.imageUrl});

  final String store;
  final String productId;
  final String title;
  final Uri? imageUrl;

  Map<String, Object?> toJson() => {'productId': productId, 'title': title, 'imageUrl': ?imageUrl?.toString()};

  /// Reads a game of [store] that [toJson] wrote, or null for anything else.
  static StoreGame? fromJson(String store, Object? json) => switch (json) {
    {'productId': final String productId, 'title': final String title} => StoreGame(
      store: store,
      productId: productId,
      title: title,
      imageUrl: imageUrlFromJson(json),
    ),
    _ => null,
  };

  static Uri? imageUrlFromJson(Map<Object?, Object?> json) =>
      json['imageUrl'] is String ? Uri.tryParse(json['imageUrl']! as String) : null;
}
