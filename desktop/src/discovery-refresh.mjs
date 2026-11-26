export function allowAutomaticRefresh(previous, socket, now) {
  return previous?.socket !== socket || now - previous.at >= 700;
}
