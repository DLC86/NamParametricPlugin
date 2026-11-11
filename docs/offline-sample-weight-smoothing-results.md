# Esperimento offline: interpolazione dei pesi per campione

**Esito: completato e superato**, workflow
[43035459355](https://github.com/DLC86/NamParametricPlugin/actions/runs/43035459355),
commit eseguito fdcc5444a61aaa67f3e707864425d8df59681267.

Si tratta di un test offline isolato: il plugin pubblicato, la release
0.5.1 e il branch main non sono stati modificati.

## Metodo

- Core HyperWaveNet/HyperA2 reale, con la patch di weight smoothing del
  prototipo 0.5.2, compilato senza JUCE.
- Una sola rete DSP eseguita alla volta. La hypernetwork calcola i pesi
  della destinazione una sola volta per ogni prova.
- 48 kHz, blocchi esterni da 257 campioni, 16.000 campioni di preroll,
  transizione smoothstep di 40 ms e 100 ms di uscita registrata.
- Confronto di aggiornamenti istantanei, ogni 4, 2, 1, 0,5, 0,1667 ms
  e **ogni singolo campione (0,02083 ms)**.
- Modelli sintetici hyper_a2_low_rank e hyper_a2_full, ciascuno in
  Full e Lite. Controlli: Gain 2 e Boost 0 iniziali, Gain 8 e Boost 1
  finali. Non è una prova con un ampli catturato dall'utente.
- Sorgenti: ingresso digitale esattamente zero e due toni a 440/3197 Hz.
  Si sottrae una risposta di riferimento a controlli invariati prima di
  misurare le differenze tra campioni.
- FFT vera, con analisi dell'energia della derivata tra 2 e 20 kHz.
  Non è una misura psicoacustica o LUFS.
- Una sola variazione di controllo per prova. Non riproduce ancora una
  lunga automazione continua con richieste ogni 20 ms.

I CSV completi delle forme d'onda e delle metriche sono nell'artifact
**offline-weight-study** della pagina GitHub Actions indicata sopra.

## Risultati con ingresso digitale zero

I picchi sono il massimo salto tra campioni dell'uscita variata,
dopo sottrazione dell'uscita della rete a controlli invariati.

| Hypernet | Taglia | Picco 0,5 ms | Picco ogni campione | Riduzione | RMS derivata 2–20 kHz 0,5 ms | RMS ogni campione | CPU per 100 ms: 0,5 ms / ogni campione |
|---|---|---:|---:|---:|---:|---:|---:|
| Low-rank | Full | 1,5002e-5 | 7,6112e-7 | −25,89 dB | 1,2299e-6 | 2,1344e-9 | 6,07 / 25,73 ms |
| Low-rank | Lite | 8,5663e-6 | 7,4320e-7 | −21,23 dB | 8,5569e-7 | 3,2486e-9 | 4,48 / 13,90 ms |
| Full | Full | 4,2846e-5 | 1,7285e-6 | −27,88 dB | 3,5225e-6 | 3,2132e-9 | 5,87 / 26,06 ms |
| Full | Lite | 1,3554e-5 | 4,5355e-7 | −29,51 dB | 1,1368e-6 | 1,6386e-9 | 4,54 / 13,94 ms |

La componente della derivata nella banda 2–20 kHz cala di circa
**48–61 dB**. Questa è una misura del segnale sintetico: non dimostra
che su un modello reale il rumore sia completamente inudibile.

## Segnale non nullo

Con due toni, i salti del segnale sono dominati dal contenuto audio
desiderato; confrontarli direttamente non equivale a misurare rumore.
L'RMS della differenza tra l'uscita a 0,5 ms e quella per campione è:

| Hypernet | Taglia | RMS residuo |
|---|---|---:|
| Low-rank | Full | 5,314e-6 |
| Low-rank | Lite | 3,828e-6 |
| Full | Full | 1,060e-5 |
| Full | Lite | 3,397e-6 |

Dopo la transizione, l'uscita converge alla stessa destinazione con
differenze finali dell'ordine di 1e-8.

## Costo CPU e indicazione tecnica

Il prototipo attuale ricarica tutti i coefficienti con una frequenza
fino a 48.000 volte al secondo, anziché circa 2.000. Chiamare
ripetutamente set_weights_ e process con un campione sacrifica
l'efficienza delle convoluzioni a blocchi.

Sulla macchina CI, HyperA2 Full ha elaborato 100 ms di audio in circa
**26 ms** per campione contro **5–6 ms** a 0,5 ms, e **4,8 ms**
a controlli fermi. La modalità per campione costa oltre **5 volte**
la normale inferenza a controlli invariati nel benchmark Full.

Il risultato supporta fortemente l'ipotesi che gli scalini
dei coefficienti generino una parte importante del rumore, ma
**non autorizza** a usare questa implementazione su hardware limitato.

**Prossimo esperimento consigliato:** interpolare il fattore alpha
direttamente nei kernel convoluzionali, possibilmente solo per i pesi
effettivamente condizionati dalla delta_map della hypernetwork,
senza riscrivere tutto il modello e senza frammentare l'inferenza in
blocchi di un solo campione. Misurare soprattutto i picchi CPU
sull'hardware di destinazione e ascoltare modelli di amplificatori reali.
