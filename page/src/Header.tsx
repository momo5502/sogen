import { useEffect } from "react";

export interface HeaderProps {
  title: string;
  description: string;
}

const image = "https://momo5502.com/sogen/preview.png";
const defaultTitle = "Sogen - Windows User Space Emulator";
const defaultDescription =
  "Sogen is a high-performance Windows user space emulator that can emulate windows processes. It is ideal for security-, DRM- or malware research.";

function setMetaTag(
  type: "name" | "property",
  key: string,
  content: string,
): void {
  const selector = `meta[${type}="${key}"]`;
  let meta = document.head.querySelector<HTMLMetaElement>(selector);

  if (!meta) {
    meta = document.createElement("meta");
    meta.setAttribute(type, key);
    document.head.appendChild(meta);
  }

  meta.content = content;
}

export function Header(props: HeaderProps) {
  useEffect(() => {
    document.title = props.title;

    setMetaTag("name", "description", props.description);
    setMetaTag("property", "og:site_name", props.title);
    setMetaTag("property", "og:title", props.title);
    setMetaTag("property", "og:description", props.description);
    setMetaTag("property", "og:locale", "en-us");
    setMetaTag("property", "og:type", "website");
    setMetaTag("property", "og:image", image);
    setMetaTag("name", "twitter:card", "summary_large_image");
    setMetaTag("name", "twitter:title", props.title);
    setMetaTag("name", "twitter:description", props.description);
    setMetaTag("name", "twitter:image", image);

    return () => {
      document.title = defaultTitle;
      setMetaTag("name", "description", defaultDescription);
      setMetaTag("property", "og:site_name", "Sogen");
      setMetaTag("property", "og:title", "Sogen");
      setMetaTag(
        "property",
        "og:description",
        "A high-performance Windows user space emulator.",
      );
      setMetaTag("property", "og:locale", "en-us");
      setMetaTag("property", "og:type", "website");
      setMetaTag("property", "og:image", image);
      setMetaTag("name", "twitter:card", "summary_large_image");
      setMetaTag("name", "twitter:title", "Sogen");
      setMetaTag(
        "name",
        "twitter:description",
        "A high-performance Windows user space emulator.",
      );
      setMetaTag("name", "twitter:image", image);
    };
  }, [props.description, props.title]);

  return null;
}
