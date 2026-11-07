/// A game the user owns in a store. [productId] names the product in that store and stays the same across its
/// builds and languages, which are chosen when installing.
class StoreGame {
  const StoreGame({required this.store, required this.productId, required this.title, this.imageUrl});

  final String store;
  final String productId;
  final String title;
  final Uri? imageUrl;
}
