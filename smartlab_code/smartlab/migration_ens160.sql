-- ============================================================
-- Migrasi: tambah kolom ENS160 (eCO2, TVOC, AQI) untuk tabel yang SUDAH ada.
-- Jalankan SEKALI di Supabase SQL Editor. Aman dijalankan ulang.
-- Data lama tidak hilang; baris lama kolom barunya bernilai null.
-- ============================================================

alter table live_readings
  add column if not exists eco2       numeric,
  add column if not exists tvoc       numeric,
  add column if not exists aqi        smallint,
  add column if not exists ens_status smallint;

alter table sensor_logs
  add column if not exists eco2 numeric,
  add column if not exists tvoc numeric,
  add column if not exists aqi  smallint;
