import sys

if __name__ == "__main__":
  header = \
"""services:
  _node_build:
    build: .
    image: ohie
    command: ["echo", "Build completed."]
"""

  node = \
"""  node{n}:
    depends_on: 
      - _node_build
    image: ohie
    container_name: node{n}
    command: [ "./Node", "8080", "_peers_docker", "node{n}:8080" ]
"""

  if len(sys.argv) != 2:
    print("Wrong usage: python " + sys.argv[0] + " <number of nodes>")
    exit

  number_of_nodes = int(sys.argv[1])

  f = open("docker-compose.yml", "w")
  f2 = open("_peers_docker", "w")
  f.write(header)
  
  f.write(node.format(n=1))
  f2.write("node{}:8080".format(1))
  for i in range(1, number_of_nodes):
    f.write(node.format(n=i+1))
    f2.write("\nnode{}:8080".format(i+1))
