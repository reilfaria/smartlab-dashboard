-- ============================================================
-- Skema Supabase untuk SmartLab Dashboard Lab APEL UNTIRTA
-- Jalankan di Supabase SQL Editor (project kamu)
-- ============================================================

-- 1) Tabel pembacaan TERKINI (1 baris per device, di-upsert tiap siklus oleh Raspi)
create table if not exists live_readings (
  device_id   text primary key,
  pm25        numeric,
  temp_in     numeric,
  hum_in      numeric,
  eco2        numeric,   -- ppm, estimasi CO2 dari ENS160
  tvoc        numeric,   -- ppb
  aqi         smallint,  -- 1..5
  ens_status  smallint,  -- 0 normal, 1 warm-up, 2 initial start-up, 3 tidak valid
  temp_out    numeric,
  hum_out     numeric,
  updated_at  timestamptz not null default now()
);

-- 2) Tabel LOG historis (1 baris tiap 10 menit, dipakai untuk download CSV)
create table if not exists sensor_logs (
  id          bigserial primary key,
  recorded_at timestamptz not null default now(),
  pm25        numeric,
  temp_in     numeric,
  hum_in      numeric,
  eco2        numeric,
  tvoc        numeric,
  aqi         smallint,
  temp_out    numeric,
  hum_out     numeric
);

create index if not exists idx_sensor_logs_recorded_at on sensor_logs (recorded_at);

-- 3) Row Level Security: dashboard (anon key) hanya boleh BACA.
--    Insert/update dari Raspberry Pi pakai service_role key (bypass RLS),
--    jadi TIDAK perlu policy insert untuk anon.
alter table live_readings enable row level security;
alter table sensor_logs enable row level security;

create policy "public read live_readings"
  on live_readings for select
  using (true);

create policy "public read sensor_logs"
  on sensor_logs for select
  using (true);

-- (opsional) aktifkan Realtime untuk live_readings di menu
-- Database > Replication di dashboard Supabase, kalau mau pakai
-- subscription realtime alih-alih polling di frontend.
