import { createFileRoute } from "@tanstack/react-router";

export const Route = createFileRoute("/")({
  head: () => ({
    meta: [
      { title: "Se atente aí — CESAR School" },
      { name: "description", content: "Plataforma de foco Pomodoro com métricas, Firebase e BlackBoard Wisdom." },
    ],
  }),
  component: Index,
});

function Index() {
  return (
    <iframe
      src="/se-atente.html"
      title="Se atente aí"
      style={{ width: "100vw", height: "100vh", border: 0, display: "block" }}
    />
  );
}
