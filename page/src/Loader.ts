import { useEffect, useState } from "react";

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

  public useLoader() {
    const [isLoading, setIsLoading] = useState(this.isLoading());

    useEffect(() => {
      function callback(loading: boolean) {
        setIsLoading(loading);
      }

      this.register(callback);

      return () => {
        this.unregister(callback);
      };
    });

    return isLoading;
  }
}

export default new Loader();
