# Smart City Sockets

Repositório com duas implementações do trabalho de cidade inteligente:

- `1_python/`: implementação em Python, incluindo o cliente Web em Streamlit.
- `2_c/`: reimplementação dos processos em C.
- `1_python/runtime_logs/`: pasta usada pelo cliente Web para guardar os logs gerados durante a execução.

## Link do vídeo apresentando o projeto

https://youtu.be/5yT9DnghPhc

## Estrutura

```text
smart_city_sockets/
├── 1_python/
├── 2_c/
├── slides/
└── README.md
```

## Documentação

- [README da implementação em Python](1_python/README.md)
- [README da implementação em C](2_c/README.md)

## Observações

- A pasta `1_python/runtime_logs/` e criada automaticamente pelo cliente Web quando os processos sao iniciados.
- Os historicos de leituras continuam sendo persistidos em `data/csv/<sensor_id>.csv` dentro de cada implementação.
