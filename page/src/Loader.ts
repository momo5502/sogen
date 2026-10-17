import { useSyncExternalStore } from "react";

type Callback = (loading: boolean) => void;

class Loader {
  private callbacks: Set<Callback> = new Set();
  private loading: boolean = false;

  public isLoading(): boolean {
    return this.loading;
  }

  public setLoading(value: boolean) {
    if (this.loading == value) {
      return;
    }

    this.loading = value;
    this.callbacks.forEach((callback) => callback(this.loading));
  }

  public register(callback: Callback): void {
    this.callbacks.add(callback);
  }

  public unregister(callback: Callback): void {
    this.callbacks.delete(callback);
  }
}

const loader = new Loader();

function subscribe(callback: Callback) {
  loader.register(callback);
  return () => loader.unregister(callback);
}

export function useLoader() {
  return useSyncExternalStore(subscribe, () => loader.isLoading());
}

export default loader;
