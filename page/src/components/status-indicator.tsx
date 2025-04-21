import { Badge } from '@/components/ui/badge'
import { CircleFill } from 'react-bootstrap-icons';

export interface StatusIndicatorProps {
    running: boolean;
};

export function StatusIndicator(props: StatusIndicatorProps) {

    const getText = () => {
        return props.running ? " Running" : " Stopped";
    };

    const getColor = () => {
        return props.running ? "bg-lime-600" : "bg-orange-600";
    }

    return (
        <Badge variant="outline"><CircleFill className={getColor() + " rounded-full mr-1 n  duration-200 ease-in-out"} color='transparent' />{getText()}</Badge>
    );
}