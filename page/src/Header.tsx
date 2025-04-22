import { Helmet } from 'react-helmet';

export interface HeaderProps {
  title: string;
}

export function Header(props: HeaderProps) {
  return (
    <Helmet>
      <title>{props.title}</title>
    </Helmet>
  );
}
