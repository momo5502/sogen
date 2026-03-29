import { useEffect } from "react";

export interface HeaderProps {
  title: string;
  description: string;
}

const defaultTitle = "Sogen - Windows User Space Emulator";
const defaultDescription =
  "Sogen is a high-performance Windows user space emulator that can emulate windows processes. It is ideal for security-, DRM- or malware research.";

function setDescriptionMetaTag(content: string): void {
  let meta = document.head.querySelector<HTMLMetaElement>(
    'meta[name="description"]',
  );

  if (!meta) {
    meta = document.createElement("meta");
    meta.setAttribute("name", "description");
    document.head.appendChild(meta);
  }

  meta.content = content;
}

export function Header(props: HeaderProps) {
  useEffect(() => {
    document.title = props.title;
    setDescriptionMetaTag(props.description);

    return () => {
      document.title = defaultTitle;
      setDescriptionMetaTag(defaultDescription);
    };
  }, [props.description, props.title]);

  return null;
}
