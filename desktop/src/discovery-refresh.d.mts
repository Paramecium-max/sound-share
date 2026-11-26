export function allowAutomaticRefresh(
  previous: { socket: WebSocket; at: number } | null,
  socket: WebSocket,
  now: number
): boolean;
