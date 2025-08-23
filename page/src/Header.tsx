import { Helmet } from "react-helmet";

export interface HeaderProps {
  title: string;
  description: string;
  preload?: string[];
}

const image = "https://momo5502.com/emulator/preview.png";

export function Header(props: HeaderProps) {
  return (
    <Helmet>
      <title>{props.title}</title>
      <meta name="description" content={props.description} />
      <meta property="og:site_name" content={props.title} />
      <meta property="og:title" content={props.title} />
      <meta property="og:description" content={props.description} />
      <meta property="og:locale" content="en-us" />
      <meta property="og:type" content="website" />
      <meta name="og:image" content={image} />
      <meta name="twitter:card" content="summary_large_image" />
      <meta name="twitter:title" content={props.title} />
      <meta name="twitter:description" content={props.description} />
      <meta name="twitter:image" content={image} />

      {props.preload?.map((l) => (
        <link
          key={`link-${l}`}
          rel="preload"
          as={l.endsWith(".js") ? "script" : "fetch"}
          crossOrigin=""
          href={`${l}${l.indexOf("?") == -1 ? "?" : "&"}cb=${import.meta.env.VITE_BUILD_TIME}`}
        />
      ))}
    </Helmet>
  );
}
