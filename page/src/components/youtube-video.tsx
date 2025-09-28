export interface FolderProps {
  id: string;
}

export function YoutubeVideo(props: FolderProps) {
  return (
    <iframe
      className="w-full h-full"
      title="Sogen Emulator Overview"
      frameBorder="0"
      allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share"
      referrerPolicy="strict-origin-when-cross-origin"
      allowFullScreen
      srcDoc={`<style>*{padding:0;margin:0;overflow:hidden}html,body{height:100%}img,div{position:absolute;width:100%;top:0;bottom:0;margin:auto;}div{height:1.5em;text-align:center;font:30px/1.5 sans-serif;color:white;overflow:visible;}span{background:red;padding:10px 20px;border-radius:15px;box-shadow: 3px 5px 10px #0000007a;}</style><a href="https://www.youtube.com/embed/${props.id}/?autoplay=1"><img src="https://img.youtube.com/vi/${props.id}/maxresdefault.jpg"><div><span>&nbsp;▶</span></div></a>`}
    ></iframe>
  );
}
